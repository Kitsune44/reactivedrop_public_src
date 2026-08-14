// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Grimowy
/**
 * @file video_ffmpeg_main.h
 * Project   : FFmpeg Video Services for Valve Source Engine
 * Component : Video Main Module
 */

#pragma once

#include "video_material.h"

#include <string>

 // -------------------------------------------------------------------
 // Forward declarations
 // -------------------------------------------------------------------

class AVFormatContext;
class AVStream;
class AVCodecContext;
class AVFrame;
class AVPacket;

class VideoSEMaterial;

 // -------------------------------------------------------------------
 // VideoFFmpegPlayer class declaration
 // -------------------------------------------------------------------

class VideoFFmpegPlayer final {
public:

    VideoFFmpegPlayer() noexcept = default;
    ~VideoFFmpegPlayer() noexcept;

    VideoFFmpegPlayer( const VideoFFmpegPlayer & ) noexcept = delete;
    VideoFFmpegPlayer &operator=( const VideoFFmpegPlayer & ) noexcept = delete;
    VideoFFmpegPlayer( VideoFFmpegPlayer && ) noexcept = default;
    VideoFFmpegPlayer &operator=( VideoFFmpegPlayer && ) noexcept = default;

    bool Init( std::string filePath ) noexcept;
    void Reset() noexcept;
    bool Update() noexcept;

    IMaterial *GetMaterial() noexcept;

    bool IsValid() const noexcept;
    const char *GetError() const noexcept;

    // -------------------------------------------------------------------
    // Playback control methods
    // -------------------------------------------------------------------

    void SetPlaying( bool enable = true ) noexcept;
    void Play() noexcept;
    void Pause() noexcept;

    void SetLooping( bool enable = true ) noexcept;
    void EnableLooping() noexcept;
    void DisableLooping() noexcept;

    bool SkipBackward() noexcept;

private:
    AVFormatContext *m_fmtCtx{ nullptr };
    AVStream *m_videoStream{ nullptr };
    AVCodecContext *m_codecCtx{ nullptr };
    AVFrame *m_frame{ nullptr };
    AVPacket *m_packet{ nullptr };
    int m_streamIndex{ -1 };

    VideoSEMaterial m_videoSEMaterial;

    bool TryReceiveFrame() noexcept;
    bool DecodeNextFrame() noexcept;
    void PrintInfo( std::string &filePath ) noexcept;

    char m_errbuf[ 64 ]{};
    bool m_isValid{ false };

    double m_videoTimeStart     { 0.0 };
    double m_videoTimePaused    { 0.0 };
    double m_videoTimeBase      { 0.0 };
    double m_videoTimeCurrent   { 0.0 };
    double m_videoTimeLastFrame { 0.0 };

    bool m_isPlaying{ false };
    bool m_isLooping{ true };
};
