/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_subtitle_track.h"

#include "ffmpeg/ffmpeg_utility.h"

namespace Media::Streaming {
namespace {

constexpr auto kFallbackCueDuration = 5 * crl::time(1000);
constexpr auto kUnknownDisplayTime = std::numeric_limits<uint32>::max();

[[nodiscard]] bool RenderableSubtitle(AVCodecID id) {
	const auto descriptor = avcodec_descriptor_get(id);
	return descriptor
		&& !(descriptor->props & AV_CODEC_PROP_BITMAP_SUB)
		&& avcodec_find_decoder(id);
}

[[nodiscard]] QString ReadMetadata(
		not_null<AVStream*> stream,
		const char *key) {
	const auto entry = av_dict_get(stream->metadata, key, nullptr, 0);
	return (entry && entry->value)
		? QString::fromUtf8(entry->value).trimmed()
		: QString();
}

void AppendCueText(SubtitleCue &cue, TextWithEntities &&text) {
	if (text.empty()) {
		return;
	} else if (!cue.text.empty()) {
		cue.text.append('\n');
	}
	cue.text.append(std::move(text));
}

} // namespace

std::vector<SubtitleTrackInfo> EnumerateSubtitleTracks(
		not_null<AVFormatContext*> format) {
	auto result = std::vector<SubtitleTrackInfo>();
	for (auto i = 0; i != int(format->nb_streams); ++i) {
		const auto stream = format->streams[i];
		const auto parameters = stream->codecpar;
		if (!parameters
			|| parameters->codec_type != AVMEDIA_TYPE_SUBTITLE) {
			continue;
		} else if (!RenderableSubtitle(parameters->codec_id)) {
			DEBUG_LOG(("Streaming Info: Skipping subtitle stream %1, "
				"can't render \"%2\"."
				).arg(i
				).arg(avcodec_get_name(parameters->codec_id)));
			continue;
		}
		result.push_back({
			.id = i,
			.language = ReadMetadata(stream, "language"),
			.title = ReadMetadata(stream, "title"),
			.codec = QString::fromUtf8(
				avcodec_get_name(parameters->codec_id)),
			.byDefault = ((stream->disposition
				& AV_DISPOSITION_DEFAULT) != 0),
			.forced = ((stream->disposition
				& AV_DISPOSITION_FORCED) != 0),
		});
	}
	return result;
}

SubtitleTrack::SubtitleTrack(
	Stream &&stream,
	Fn<void(std::vector<SubtitleCue>&&)> cuesDecoded)
: _stream(std::move(stream))
, _cuesDecoded(std::move(cuesDecoded)) {
	Expects(_stream.codec != nullptr);
	Expects(_cuesDecoded != nullptr);
}

SubtitleTrack::~SubtitleTrack() = default;

int SubtitleTrack::streamIndex() const {
	return _stream.index;
}

void SubtitleTrack::process(
		std::vector<FFmpeg::Packet> &&packets,
		crl::time shift) {
	auto cues = std::vector<SubtitleCue>();
	for (const auto &packet : packets) {
		if (packet.empty()) {
			flushPending(cues);
		} else {
			readPacket(packet, shift, cues);
		}
	}
	if (!cues.empty()) {
		_cuesDecoded(std::move(cues));
	}
}

void SubtitleTrack::readPacket(
		const FFmpeg::Packet &packet,
		crl::time shift,
		std::vector<SubtitleCue> &cues) {
	const auto position = FFmpeg::PacketPosition(packet, _stream.timeBase);
	if (position == kTimeUnknown) {
		return;
	}
	auto subtitle = AVSubtitle();
	auto got = 0;
	const auto error = FFmpeg::AvErrorWrap(avcodec_decode_subtitle2(
		_stream.codec.get(),
		&subtitle,
		&got,
		&packet.fields()));
	if (error) {
		if (!_stream.invalidDataPackets++) {
			FFmpeg::LogError(u"avcodec_decode_subtitle2"_q, error);
		}
		return;
	}
	_stream.invalidDataPackets = 0;
	if (!got) {
		return;
	}
	const auto guard = gsl::finally([&] {
		avsubtitle_free(&subtitle);
	});
	const auto start = shift + position;

	// Some formats say "show this until further notice" and then send an
	// empty subtitle to clear the screen. Such a cue waits in '_pending'
	// until the packet that ends it arrives.
	closePending(start, cues);
	if (!subtitle.num_rects) {
		return;
	}
	auto cue = SubtitleCue();
	auto open = false;
	const auto from = crl::time(subtitle.start_display_time);
	const auto till = (subtitle.end_display_time == kUnknownDisplayTime)
		? crl::time(0)
		: crl::time(subtitle.end_display_time);
	const auto duration = FFmpeg::PacketDuration(packet, _stream.timeBase);
	cue.from = start + from;
	if (till > from) {
		cue.till = start + till;
	} else if (duration > 0 && duration != kTimeUnknown) {
		cue.till = cue.from + duration;
	} else {
		cue.till = cue.from + kFallbackCueDuration;
		open = true;
	}
	fillCue(cue, subtitle);
	if (cue.empty()) {
		return;
	} else if (open) {
		_pending = std::move(cue);
	} else {
		cues.push_back(std::move(cue));
	}
}

void SubtitleTrack::fillCue(SubtitleCue &cue, const AVSubtitle &subtitle) {
	for (auto i = 0; i != int(subtitle.num_rects); ++i) {
		const auto rect = subtitle.rects[i];
		if (!rect) {
			continue;
		} else if (rect->type == SUBTITLE_ASS && rect->ass) {
			AppendCueText(cue, ParseAssEvent(QString::fromUtf8(rect->ass)));
		} else if (rect->type == SUBTITLE_TEXT && rect->text) {
			AppendCueText(
				cue,
				ParsePlainSubtitleText(QString::fromUtf8(rect->text)));
		}
	}
}

void SubtitleTrack::closePending(
		crl::time position,
		std::vector<SubtitleCue> &cues) {
	if (!_pending) {
		return;
	} else if (_pending->till > position) {
		_pending->till = std::max(position, _pending->from + 1);
	}
	cues.push_back(*base::take(_pending));
}

void SubtitleTrack::flushPending(std::vector<SubtitleCue> &cues) {
	if (_pending) {
		cues.push_back(*base::take(_pending));
	}
}

} // namespace Media::Streaming
