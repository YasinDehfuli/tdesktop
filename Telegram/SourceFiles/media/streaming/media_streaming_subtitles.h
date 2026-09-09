/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_common.h"

namespace Media::Streaming {

struct SubtitleTrackInfo {
	int id = kSubtitlesOff;
	QString language;
	QString title;
	QString codec;
	bool byDefault = false;
	bool forced = false;

	friend inline bool operator==(
		const SubtitleTrackInfo &,
		const SubtitleTrackInfo &) = default;
};

struct SubtitleCue {
	crl::time from = 0;
	crl::time till = 0;
	TextWithEntities text;

	[[nodiscard]] bool empty() const {
		return text.empty();
	}
};

// Holds the cues decoded so far and tells which of them are visible now.
//
// Knows nothing about FFmpeg or about the file the cues came from, so the
// same timeline serves embedded streams and, later, external subtitle files.
// Lives on the main thread, cues are added from the decoding thread hop.
class SubtitleTimeline final {
public:
	void add(std::vector<SubtitleCue> &&cues);
	void clear();

	// Returns true if the set of visible cues has changed.
	bool moveTo(crl::time position);

	[[nodiscard]] const std::vector<SubtitleCue> &visible() const;

private:
	void removeStaleCues();

	std::deque<SubtitleCue> _cues;
	std::vector<SubtitleCue> _visible;
	crl::time _position = kTimeUnknown;
	int _next = 0;

};

// Picks the track a player should start with when the user made no choice:
// an explicitly forced track wins over a default one, and when the file
// marks nothing we still show the only track it has.
[[nodiscard]] int ChooseSubtitleTrack(
	const std::vector<SubtitleTrackInfo> &tracks);

// Converts one ASS/SSA event into displayable text. Accepts both the packet
// format FFmpeg produces for AV_CODEC_ID_ASS and text subtitles it converts
// to ASS ("ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text")
// and a legacy full "Dialogue:" line.
[[nodiscard]] TextWithEntities ParseAssEvent(const QString &event);

[[nodiscard]] TextWithEntities ParsePlainSubtitleText(const QString &text);

} // namespace Media::Streaming
