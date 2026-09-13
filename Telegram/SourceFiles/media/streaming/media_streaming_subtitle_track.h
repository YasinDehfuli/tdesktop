/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_subtitles.h"
#include "media/streaming/media_streaming_utility.h"

namespace Media::Streaming {

// What the File thread hands over for the subtitles of one playback.
struct SubtitlesSource {
	Stream stream;
	std::vector<SubtitleTrackInfo> tracks;
	int chosenId = kSubtitlesOff;
};

[[nodiscard]] std::vector<SubtitleTrackInfo> EnumerateSubtitleTracks(
	not_null<AVFormatContext*> format);

class SubtitleTrack final {
public:
	// Called from the File thread. The callback is assumed to be thread-safe.
	SubtitleTrack(
		Stream &&stream,
		Fn<void(std::vector<SubtitleCue>&&)> cuesDecoded);

	// Thread-safe.
	[[nodiscard]] int streamIndex() const;

	// Called from the File thread.
	void process(std::vector<FFmpeg::Packet> &&packets, crl::time shift);

	// Called from the main thread.
	~SubtitleTrack();

private:
	void readPacket(
		const FFmpeg::Packet &packet,
		crl::time shift,
		std::vector<SubtitleCue> &cues);
	void fillCue(SubtitleCue &cue, const AVSubtitle &subtitle);
	void closePending(crl::time position, std::vector<SubtitleCue> &cues);
	void flushPending(std::vector<SubtitleCue> &cues);

	Stream _stream;
	const Fn<void(std::vector<SubtitleCue>&&)> _cuesDecoded;

	// A cue whose end is not known yet, waiting for the packet that ends it.
	std::optional<SubtitleCue> _pending;

};

} // namespace Media::Streaming
