/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_subtitles.h"

namespace Media::Streaming {
namespace {

constexpr auto kKeepStaleCuesFor = 5 * crl::time(1000);
constexpr auto kAssFieldsBeforeText = 8;
constexpr auto kAssDialogueFieldsBeforeText = 9;

[[nodiscard]] bool CueEarlier(const SubtitleCue &a, const SubtitleCue &b) {
	return a.from < b.from;
}

// One formatting range opened by an ASS override like {\i1} and closed by
// {\i0}, by {\r}, or by the end of the event.
struct OpenFormat {
	EntityType type = EntityType::Invalid;
	int offset = 0;
};

[[nodiscard]] int AssOverrideArgument(const QString &block, int from) {
	auto value = 0;
	auto digits = 0;
	while (from + digits < block.size() && block[from + digits].isDigit()) {
		value = (value * 10) + block[from + digits].digitValue();
		++digits;
	}
	return digits ? value : -1;
}

class AssTextParser final {
public:
	[[nodiscard]] TextWithEntities parse(const QString &text);

private:
	void handleOverrideBlock(const QString &block);
	void toggle(EntityType type, bool enabled);
	void closeAll();
	void append(QChar character);
	void finalize();

	TextWithEntities _result;
	std::vector<OpenFormat> _open;
	bool _drawing = false;

};

TextWithEntities AssTextParser::parse(const QString &text) {
	const auto size = text.size();
	for (auto i = 0; i != size; ++i) {
		const auto character = text[i];
		if (character == '{') {
			const auto closing = text.indexOf('}', i + 1);
			if (closing < 0) {
				break;
			}
			handleOverrideBlock(text.mid(i + 1, closing - i - 1));
			i = closing;
		} else if (character == '\\' && i + 1 != size) {
			const auto next = text[++i];
			if (next == 'N') {
				append('\n');
			} else if (next == 'n') {
				// A soft break, honoured by libass only when the style
				// disables wrapping. We always wrap the rendered text
				// ourselves, so it becomes a space, like libass does.
				append(' ');
			} else if (next == 'h') {
				append(QChar(0x00A0));
			} else if (next == '\\' || next == '{' || next == '}') {
				append(next);
			} else {
				append(character);
				append(next);
			}
		} else if (character == '\r') {
		} else {
			append(character);
		}
	}
	finalize();
	return std::move(_result);
}

void AssTextParser::finalize() {
	closeAll();

	auto size = int(_result.text.size());
	while (size > 0 && _result.text[size - 1].isSpace()) {
		--size;
	}
	_result.text.truncate(size);

	// Nested overrides close from the inside out, so the entities are
	// collected out of order, while the block parser walks them once
	// from left to right.
	ranges::stable_sort(
		_result.entities,
		ranges::less(),
		&EntityInText::offset);
	for (auto &entity : _result.entities) {
		entity.updateTextEnd(size);
	}
	_result.entities.erase(
		ranges::remove_if(_result.entities, [&](const EntityInText &entity) {
			return !entity.validForText(size);
		}),
		_result.entities.end());
}

void AssTextParser::handleOverrideBlock(const QString &block) {
	for (auto i = block.indexOf('\\'); i >= 0; i = block.indexOf('\\', i)) {
		++i;
		if (i == block.size()) {
			break;
		}
		const auto tag = block[i++];
		const auto argument = AssOverrideArgument(block, i);
		if (tag == 'i') {
			toggle(EntityType::Italic, argument > 0);
		} else if (tag == 'b') {
			toggle(EntityType::Bold, argument > 0);
		} else if (tag == 'u') {
			toggle(EntityType::Underline, argument > 0);
		} else if (tag == 's') {
			toggle(EntityType::StrikeOut, argument > 0);
		} else if (tag == 'p') {
			_drawing = (argument > 0);
		} else if (tag == 'r') {
			closeAll();
		}
	}
}

void AssTextParser::toggle(EntityType type, bool enabled) {
	const auto i = ranges::find(_open, type, &OpenFormat::type);
	if (enabled) {
		if (i == end(_open)) {
			_open.push_back({ type, int(_result.text.size()) });
		}
		return;
	} else if (i == end(_open)) {
		return;
	}
	const auto length = int(_result.text.size()) - i->offset;
	if (length > 0) {
		_result.entities.push_back({ type, i->offset, length });
	}
	_open.erase(i);
}

void AssTextParser::closeAll() {
	while (!_open.empty()) {
		toggle(_open.back().type, false);
	}
}

void AssTextParser::append(QChar character) {
	if (_drawing || (_result.text.isEmpty() && character.isSpace())) {
		return;
	}
	_result.text.append(character);
}

} // namespace

void SubtitleTimeline::add(std::vector<SubtitleCue> &&cues) {
	if (cues.empty()) {
		return;
	}
	for (auto &cue : cues) {
		_cues.push_back(std::move(cue));
	}
	const auto from = begin(_cues) + _next;
	if (!std::is_sorted(from, end(_cues), CueEarlier)) {
		std::stable_sort(from, end(_cues), CueEarlier);
	}
}

void SubtitleTimeline::clear() {
	_cues.clear();
	_visible.clear();
	_position = kTimeUnknown;
	_next = 0;
}

bool SubtitleTimeline::moveTo(crl::time position) {
	if (position == kTimeUnknown) {
		return false;
	}
	auto changed = false;
	if (_position != kTimeUnknown && position < _position) {
		changed = !_visible.empty();
		_visible.clear();
		_next = 0;
	}
	_position = position;
	const auto count = int(_cues.size());
	while (_next != count && _cues[_next].from <= position) {
		const auto &cue = _cues[_next++];
		if (cue.till > position) {
			_visible.push_back(cue);
			changed = true;
		}
	}
	const auto expired = ranges::remove_if(_visible, [&](
			const SubtitleCue &cue) {
		return (cue.till <= position);
	});
	if (expired != end(_visible)) {
		_visible.erase(expired, end(_visible));
		changed = true;
	}
	removeStaleCues();
	return changed;
}

const std::vector<SubtitleCue> &SubtitleTimeline::visible() const {
	return _visible;
}

void SubtitleTimeline::removeStaleCues() {
	auto remove = 0;
	while (remove != _next
		&& _cues[remove].till + kKeepStaleCuesFor < _position) {
		++remove;
	}
	if (remove > 0) {
		_cues.erase(begin(_cues), begin(_cues) + remove);
		_next -= remove;
	}
}

int ChooseSubtitleTrack(const std::vector<SubtitleTrackInfo> &tracks) {
	if (tracks.empty()) {
		return kSubtitlesOff;
	}
	const auto forced = ranges::find(
		tracks,
		true,
		&SubtitleTrackInfo::forced);
	if (forced != end(tracks)) {
		return forced->id;
	}
	const auto byDefault = ranges::find(
		tracks,
		true,
		&SubtitleTrackInfo::byDefault);
	if (byDefault != end(tracks)) {
		return byDefault->id;
	}
	return (tracks.size() == 1) ? tracks.front().id : kSubtitlesOff;
}

TextWithEntities ParseAssEvent(const QString &event) {
	auto text = QStringView(event).trimmed();
	auto fields = kAssFieldsBeforeText;
	if (text.startsWith(u"Dialogue:", Qt::CaseInsensitive)) {
		text = text.mid(9);
		fields = kAssDialogueFieldsBeforeText;
	}
	for (auto i = 0; i != fields; ++i) {
		const auto comma = text.indexOf(',');
		if (comma < 0) {
			break;
		}
		text = text.mid(comma + 1);
	}
	return AssTextParser().parse(text.toString());
}

TextWithEntities ParsePlainSubtitleText(const QString &text) {
	auto result = text;
	result.replace(u"\r\n"_q, u"\n"_q);
	result.replace(QChar('\r'), QChar('\n'));
	return TextWithEntities::Simple(result.trimmed());
}

} // namespace Media::Streaming
