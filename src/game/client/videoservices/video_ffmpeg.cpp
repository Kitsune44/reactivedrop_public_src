// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Grimowy
/**
 * @file video_ffmpeg_main.cpp
 * Project   : FFmpeg Video Services for Valve Source Engine
 * Component : Video Main Module
 * 
 * @brief Core VideoFFmpegPlayer class for Valve Source Engine.
 * Provides video initialization, decoding, playback control
 * and material update functionalities with robust error handling.
 */

#include "cbase.h"
#include "video_ffmpeg.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}


// -------------------------------------------------------------------
// Destructor and Reset
// -------------------------------------------------------------------

VideoFFmpegPlayer::~VideoFFmpegPlayer() noexcept {
    Reset();
}

/**
 * @brief Reset internal state and free all allocated FFmpeg resources.
 *
 * This is idempotent and safe to call multiple times.
 */
void VideoFFmpegPlayer::Reset() noexcept {
    m_isPlaying = false;
    m_isValid = false;
    m_isLooping = true;

    m_videoSEMaterial.Reset();

    av_packet_free( &m_packet );
    av_frame_free( &m_frame );
    avcodec_free_context( &m_codecCtx );
    avformat_close_input( &m_fmtCtx );

    m_fmtCtx = nullptr;
    m_videoStream = nullptr;
    m_codecCtx = nullptr;
    m_frame = nullptr;
    m_packet = nullptr;
    m_streamIndex = -1;

    m_videoTimeStart = 0.0;
    m_videoTimePaused = 0.0;
    m_videoTimeBase = 0.0;
    m_videoTimeCurrent = 0.0;
    m_videoTimeLastFrame = 0.0;

    memset( m_errbuf, 0, sizeof( m_errbuf ) );
}

// -------------------------------------------------------------------
// Initialization
// -------------------------------------------------------------------

/**
 * @brief Initialize video playback from the specified file path.
 *
 * Opens the media file, finds video stream, sets up codec context,
 * allocates necessary frames and packets, and performs initial decode.
 *
 * @param filePath Path to video file.
 * @return true if initialization succeeded and first frame decoded.
 * @return false on any error, error details available via GetError().
 */
bool VideoFFmpegPlayer::Init( std::string filePath ) noexcept {

    m_fmtCtx = avformat_alloc_context();
    if ( !m_fmtCtx ) {
        snprintf( m_errbuf, sizeof( m_errbuf ), "Fail: Allocate an AVFormatContext" );
        return false;
    }
    m_fmtCtx->flags |= AVFMT_FLAG_FAST_SEEK;


    int ret = avformat_open_input( &m_fmtCtx, filePath.c_str(), nullptr, nullptr );
    if ( ret < 0 ) {
        av_strerror( ret, m_errbuf, sizeof( m_errbuf ) );
        return false;
    }

    ret = avformat_find_stream_info( m_fmtCtx, nullptr );
    if ( ret < 0 ) {
        av_strerror( ret, m_errbuf, sizeof( m_errbuf ) );
        return false;
    }

    const AVCodec *codec = nullptr;
    m_streamIndex = av_find_best_stream( m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0 );
    if ( m_streamIndex < 0 ) {
        av_strerror( m_streamIndex, m_errbuf, sizeof( m_errbuf ) );
        return false;
    }

    m_videoStream = m_fmtCtx->streams[ m_streamIndex ];
    m_videoTimeBase = av_q2d( m_videoStream->time_base );

    m_codecCtx = avcodec_alloc_context3( codec );
    if ( !m_codecCtx ) {
        snprintf( m_errbuf, sizeof( m_errbuf ), "Failed to allocate codec context" );
        return false;
    }

    ret = avcodec_parameters_to_context( m_codecCtx, m_videoStream->codecpar );
    if ( ret < 0 ) {
        av_strerror( ret, m_errbuf, sizeof( m_errbuf ) );
        return false;
    }

    // Enable multi-threading for decoding
    m_codecCtx->thread_count = 0;
    m_codecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    //m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    //m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;

    ret = avcodec_open2( m_codecCtx, codec, nullptr );
    if ( ret < 0 ) {
        av_strerror( ret, m_errbuf, sizeof( m_errbuf ) );
        return false;
    }

    m_packet = av_packet_alloc();
    if ( !m_packet ) {
        snprintf( m_errbuf, sizeof( m_errbuf ), "Failed to allocate AVPacket" );
        return false;
    }

    m_frame = av_frame_alloc();
    if ( !m_frame ) {
        snprintf( m_errbuf, sizeof( m_errbuf ), "Failed to allocate AVFrame" );
        return false;
    }

    // Decode first frame to validate stream and format
    Play();
    if ( !DecodeNextFrame() ) {
        snprintf( m_errbuf, sizeof( m_errbuf ), "Failed to decode initial frame" );
        return false;
    }
    m_videoTimeLastFrame = 0.0;

    PrintInfo( filePath );

    const AVPixelFormat pixFmt = static_cast< AVPixelFormat >( m_frame->format );
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get( pixFmt );
    int bitDepth = desc->comp[ 0 ].depth;

    // Validate pixel format and frame dimensions for expected planar YUV 3-component format
    if ( !desc
        || !( desc->flags & AV_PIX_FMT_FLAG_PLANAR )
        || desc->nb_components != 3
        || strncmp( desc->name, "yuv", 3 ) != 0
        || ( ( bitDepth == 8 && m_frame->width != m_frame->linesize[ 0 ] )
            || ( bitDepth > 8 && m_frame->width != ( m_frame->linesize[ 0 ] >> 1 ) ) )
        )
    {
        snprintf( m_errbuf, sizeof( m_errbuf ), "Unsupported pixel format or dimensions: %s", av_get_pix_fmt_name( pixFmt ) );
        return false;
    }

    int colorRange = m_frame->color_range;
    int colorSpace = m_frame->colorspace;
    int colorTransferC = m_frame->color_trc;
    int colorPrimaries = m_frame->color_primaries;

    // Compute chroma plane sizes according to pixel format subsampling
    const size_t wY = static_cast< size_t >( m_frame->width );
    const size_t hY = static_cast< size_t >( m_frame->height );
    const size_t log2_chroma_w = static_cast< size_t >( desc->log2_chroma_w );
    const size_t log2_chroma_h = static_cast< size_t >( desc->log2_chroma_h );

    const size_t wUV = ( wY + ( 1 << log2_chroma_w ) - 1 ) >> log2_chroma_w;
    const size_t hUV = ( hY + ( 1 << log2_chroma_h ) - 1 ) >> log2_chroma_h;

    uint8_t *&srcY = m_frame->data[ 0 ];
    uint8_t *&srcU = m_frame->data[ 1 ];
    uint8_t *&srcV = m_frame->data[ 2 ];

    // Initialize material
    m_videoSEMaterial.Init( wY, hY, wUV, hUV, srcY, srcU, srcV, bitDepth, colorRange, colorSpace, colorTransferC, colorPrimaries );

    // Perform initial update to upload textures to GPU
    m_videoSEMaterial.Update();
    Pause();

    m_isValid = true;
    return true;
}

// -------------------------------------------------------------------
// Playback Control
// -------------------------------------------------------------------

/**
 * @brief Set playback state.
 *
 * When starting playback, records start time.
 * When pausing, accumulates paused duration.
 *
 * @param enable True to start playing, false to pause.
 */
void VideoFFmpegPlayer::SetPlaying( bool enable ) noexcept {
    if ( m_isPlaying != enable ) {
        if ( enable ) {
            m_videoTimeStart = Plat_FloatTime();
        }
        else {
            m_videoTimePaused += Plat_FloatTime() - m_videoTimeStart;
        }
        m_isPlaying = enable;
    }
}

/** @brief Alias for SetPlaying(true). */
void VideoFFmpegPlayer::Play() noexcept {
    SetPlaying( true );
}

/** @brief Alias for SetPlaying(false). */
void VideoFFmpegPlayer::Pause() noexcept {
    SetPlaying( false );
}

/**
 * @brief Enable or disable looping.
 * @param enable True to enable looping, false to disable.
 */
void VideoFFmpegPlayer::SetLooping( bool enable ) noexcept {
    m_isLooping = enable;
}

/** @brief Enable looping explicitly. */
void VideoFFmpegPlayer::EnableLooping() noexcept {
    SetLooping( true );
}

/** @brief Disable looping explicitly. */
void VideoFFmpegPlayer::DisableLooping() noexcept {
    SetLooping( false );
}

// -------------------------------------------------------------------
// Seeking
// -------------------------------------------------------------------

/**
 * @brief Seek to the beginning of the video stream.
 *
 * Flushes codec buffers and resets timing state.
 *
 * @return true if seek succeeded, false otherwise.
 */
bool VideoFFmpegPlayer::SkipBackward() noexcept {
    int ret = av_seek_frame( m_fmtCtx, m_streamIndex, 0, AVSEEK_FLAG_BACKWARD );
    if ( ret < 0 ) {
        m_isValid = false;
        av_strerror( ret, m_errbuf, sizeof( m_errbuf ) );
        return false;
    }
    avcodec_flush_buffers( m_codecCtx );

    m_videoTimeStart = Plat_FloatTime();
    m_videoTimePaused = 0.0;
    m_videoTimeCurrent = 0.0;
    m_videoTimeLastFrame = 0.0;

    return true;
}

// -------------------------------------------------------------------
// Accessors
// -------------------------------------------------------------------

/**
 * @brief Retrieve the material representing the current video frame.
 * @return Pointer to IMaterial interface.
 */
IMaterial *VideoFFmpegPlayer::GetMaterial() noexcept {
    return m_videoSEMaterial.GetMaterial();
}

/**
 * @brief Get textual description of the last error.
 * @return Null-terminated error string.
 */
const char *VideoFFmpegPlayer::GetError() const noexcept {
    return m_errbuf;
}

/**
 * @brief Check if the player is in a valid state for playback.
 * @return true if valid, false if any error occurred.
 */
bool VideoFFmpegPlayer::IsValid() const noexcept {
    return m_isValid;
}

// -------------------------------------------------------------------
// Update & Frame Decoding
// -------------------------------------------------------------------

/**
 * @brief Update the video playback state and decode frames as needed.
 *
 * Decodes frames only if playing and if current video time advances beyond last decoded frame.
 * Updates material with new frame data on successful decode.
 *
 * @return true if still valid and updated, false if invalid or error occurred.
 */
bool VideoFFmpegPlayer::Update() noexcept {
    if ( !m_isValid ) return false;
    if ( !m_isPlaying ) return true;

    // Compute current video time including paused duration
    m_videoTimeCurrent = ( Plat_FloatTime() - m_videoTimeStart ) + m_videoTimePaused;

    // Decode next frame only if next frame timestamp >= current time
    if ( m_videoTimeLastFrame <= m_videoTimeCurrent && DecodeNextFrame() ) {
        m_videoSEMaterial.Update();
    }

    return true;
}

/**
 * @brief Decode next frame, returning when a frame timestamp is >= current video time.
 *
 * Continuously reads packets, sends them to the decoder, and receives decoded frames.
 * Handles end-of-file by looping if enabled.
 *
 * @return true if a suitable frame was decoded, false on error or if looping failed.
 */
bool VideoFFmpegPlayer::DecodeNextFrame() noexcept {
    int ret = 0;

    while ( true ) {
        // Attempt to receive decoded frames from decoder until none available
        while ( TryReceiveFrame() ) {
            m_videoTimeLastFrame = m_videoTimeBase * m_frame->pts;
            if ( m_videoTimeLastFrame >= m_videoTimeCurrent ) {
                return true;
            }
        }

        // Read next packet from input
        ret = av_read_frame( m_fmtCtx, m_packet );
        if ( ret == AVERROR_EOF ) {
            // Flush decoder by sending null packet
            avcodec_send_packet( m_codecCtx, nullptr );
            // Loop or end
            if ( m_isLooping && !SkipBackward() ) {
                return false;
            }
            continue;
        }
        if ( ret < 0 ) {
            m_isValid = false;
            av_strerror( ret, m_errbuf, sizeof( m_errbuf ) );
            return false;
        }

        // Send packets only from the video stream
        if ( m_packet->stream_index == m_streamIndex ) {
            ret = avcodec_send_packet( m_codecCtx, m_packet );
            av_packet_unref( m_packet );
            if ( ret < 0 ) {
                m_isValid = false;
                av_strerror( ret, m_errbuf, sizeof( m_errbuf ) );
                return false;
            }
        }
        else {
            // Discard packets from other streams
            av_packet_unref( m_packet );
        }
    }
}

/**
 * @brief Attempt to receive a decoded frame from the codec context.
 *
 * @return true if a frame was received, false if no frame available or on error.
 */
bool VideoFFmpegPlayer::TryReceiveFrame() noexcept {
    int ret = avcodec_receive_frame( m_codecCtx, m_frame );
    if ( ret == 0 ) return true;
    if ( ret == AVERROR( EAGAIN ) ) return false;

    m_isValid = false;
    av_strerror( ret, m_errbuf, sizeof( m_errbuf ) );
    return false;
}

// -------------------------------------------------------------------
// Debugging and logging
// -------------------------------------------------------------------

/**
 * @brief Log video and codec information for debugging.
 *
 * @param filePath -  Path to the opened video file.
 */
void VideoFFmpegPlayer::PrintInfo( std::string &filePath ) noexcept {
    const double dur = ( m_fmtCtx->duration != AV_NOPTS_VALUE ) ? m_fmtCtx->duration / static_cast< double >( AV_TIME_BASE ) : -1.0;
    const AVRational fps = av_guess_frame_rate( m_fmtCtx, m_videoStream, nullptr );
    const AVCodecDescriptor *desc = avcodec_descriptor_get( m_videoStream->codecpar->codec_id );
    const char *pixFmt = av_get_pix_fmt_name( static_cast< AVPixelFormat >( m_videoStream->codecpar->format ) );

    ConColorMsg( Color( 77, 166, 255, 255 ), "\n\n[FFMPEG VIDEO]\n" );
    ConColorMsg( Color( 77, 166, 255, 150 ), "  Source:\n" );
    ConColorMsg( Color( 153, 204, 255, 255 ), "    File: %s\n", filePath.c_str() );
    ConColorMsg( Color( 153, 204, 255, 150 ), "    Resolution:      %d x %d px\n", m_videoStream->codecpar->width, m_videoStream->codecpar->height );
    ConColorMsg( Color( 153, 204, 255, 255 ), "    Frame rate:      %.2f fps\n", ( fps.den != 0 ) ? static_cast< double >( fps.num ) / fps.den : 0.0 );
    ConColorMsg( Color( 153, 204, 255, 150 ), "    Duration:        %.2f s\n", dur );
    if ( desc != nullptr ) {
        ConColorMsg( Color( 153, 204, 255, 255 ), "    Codec:           %s (%s)\n", desc->name, desc->long_name );
    }
    else {
        ConColorMsg( Color( 153, 204, 255, 255 ), "    Codec:           unknown\n" );
    }
    ConColorMsg( Color( 153, 204, 255, 150 ), "    Pixel Format:    %s\n", pixFmt ? pixFmt : "unknown" );

    const AVCodec *decoder = m_codecCtx->codec;
    const char *range_str = av_color_range_name( m_frame->color_range );
    const char *space_str = av_color_space_name( m_frame->colorspace );
    const char *primaries_str = av_color_primaries_name( m_frame->color_primaries );
    const char *transfer_str = av_color_transfer_name( m_frame->color_trc );
    pixFmt = av_get_pix_fmt_name( static_cast< AVPixelFormat >( m_frame->format ) );

    ConColorMsg( Color( 77, 166, 255, 150 ), "  Decoder:\n" );
    if ( decoder != nullptr ) {
        ConColorMsg( Color( 153, 204, 255, 255 ), "    Decoder:         %s (%s)\n", decoder->name ? decoder->name : "unknown", decoder->long_name ? decoder->long_name : "unknown" );
    }
    else {
        ConColorMsg( Color( 153, 204, 255, 255 ), "    Decoder:         unknown\n" );
    }
    ConColorMsg( Color( 153, 204, 255, 150 ), "    Pixel Format:    %s\n", pixFmt ? pixFmt : "unknown" );
    ConColorMsg( Color( 153, 204, 255, 255 ), "    Color range:     %s\n", range_str ? range_str : "unknown" );
    ConColorMsg( Color( 153, 204, 255, 150 ), "    Color space:     %s\n", space_str ? space_str : "unknown" );
    ConColorMsg( Color( 153, 204, 255, 255 ), "    Color Primaries: %s\n", primaries_str ? primaries_str : "unknown" );
    ConColorMsg( Color( 153, 204, 255, 150 ), "    Color Transfer:  %s\n", transfer_str ? transfer_str : "unknown" );
}
