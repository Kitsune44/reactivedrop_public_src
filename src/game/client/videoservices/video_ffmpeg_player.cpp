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
#include "video_ffmpeg_player.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_dxva2.h>
}

#ifdef _WIN32
// Software fallback switch for the DXVA2 hardware path.
ConVar rd_video_hwdec( "rd_video_hwdec", "1", FCVAR_ARCHIVE, "Use DXVA2 hardware decoding for FFmpeg videos" );

// Maps the FFmpeg decoder surface format to its D3D9 FOURCC (these constants
// are not defined in the Windows SDK headers).
static D3DFORMAT SwFormatToD3DFormat( AVPixelFormat fmt )
{
	switch ( fmt )
	{
	case AV_PIX_FMT_NV12: return static_cast<D3DFORMAT>( MAKEFOURCC( 'N', 'V', '1', '2' ) );
	case AV_PIX_FMT_P010: return static_cast<D3DFORMAT>( MAKEFOURCC( 'P', '0', '1', '0' ) );
	case AV_PIX_FMT_P016: return static_cast<D3DFORMAT>( MAKEFOURCC( 'P', '0', '1', '6' ) );
	default:			  return D3DFMT_UNKNOWN;
	}
}
#endif


// Single-video assumption: the one active player instance owns the reset
// callback registered in RD_D3D9 (the HW video path is per-menu background).
static VideoFFmpegPlayer *s_pHWResetOwner = nullptr;


// -------------------------------------------------------------------
// FFmpeg log forwarding (diagnostics)
// -------------------------------------------------------------------

// Forward FFmpeg's own av_log output (>= AV_LOG_VERBOSE) into the game
// console so hwaccel selection/fallback reasons are visible.
static void VideoFFmpegLogCallback( void * /*ptr*/, int level, const char *fmt, va_list vl )
{
	if ( level > AV_LOG_VERBOSE )
		return;

	char buf[ 512 ];
	vsnprintf( buf, sizeof( buf ), fmt, vl );

	// av_log messages end with '\n'; trim it so Warning() formats cleanly.
	const size_t len = strlen( buf );
	if ( len > 0 && buf[ len - 1 ] == '\n' )
		buf[ len - 1 ] = '\0';

	Warning( "[ffmpeg] %s\n", buf );
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

	// Tear down the GPU display path (video processor + RGB RT) first - it was
	// created from the DXVA2 device manager, so it must go while the manager
	// is still alive.
	m_videoSEMaterial.Reset();

	// Release decoded frames while the decoder/device infrastructure is alive.
	av_frame_free( &m_frame );
	avcodec_free_context( &m_codecCtx );
	av_packet_free( &m_packet );
	avformat_close_input( &m_fmtCtx );

	// Now release the remaining DXVA2 state and device manager.
	ShutdownHardwareDecode();

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

	av_log_set_level( AV_LOG_VERBOSE );
	av_log_set_callback( VideoFFmpegLogCallback );

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

	// The software fallback decoder (libdav1d for AV1) - always used by the
	// SW paths below; only the HW attempt may switch to the native decoder.
	const AVCodec *swCodec = codec;

	// av_find_best_stream() may pick libdav1d for AV1, which has NO DXVA2
	// hwaccel in FFmpeg - hardware decode requires the native "av1" decoder.
	if ( rd_video_hwdec.GetBool() && codec && codec->id == AV_CODEC_ID_AV1 )
	{
		const AVCodec *nativeAv1 = avcodec_find_decoder_by_name( "av1" );
		if ( nativeAv1 )
			codec = nativeAv1;
		else
			Warning( "[video] DXVA2: native AV1 decoder (av1) not available, hardware decode disabled\n" );
	}

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

	m_swCodec = codec;

	// Try DXVA2 hardware decode first (hwaccel attached to the software
	// decoder, the same way "-hwaccel dxva2" works); the existing software
	// decoder remains the fallback when hardware is unavailable.
	if ( InitHardwareDecode() )
	{
		m_bHardwareDecode = true;
		// Only register the reset observer once the player has accepted the
		// hardware path (the callback is owned by the single active player).
		s_pHWResetOwner = this;
		RD_D3D9::AddResetCallback( &VideoFFmpegPlayer::DeviceResetCallback );
	}
	else
	{
		Warning( "[video] DXVA2 hardware decode unavailable, falling back to software decode\n" );
		m_bHardwareDecode = false;
		ShutdownHardwareDecode();   // release any partially created HW state

		// The HW attempt may have replaced the context with the native AV1
		// decoder, which has no software decode path in this build - rebuild
		// the context for the software decoder with multithreading enabled.
		avcodec_free_context( &m_codecCtx );
		m_codecCtx = avcodec_alloc_context3( swCodec );
		if ( !m_codecCtx ) {
			snprintf( m_errbuf, sizeof( m_errbuf ), "Failed to allocate codec context" );
			return false;
		}
		if ( avcodec_parameters_to_context( m_codecCtx, m_videoStream->codecpar ) < 0 ) {
			snprintf( m_errbuf, sizeof( m_errbuf ), "Failed to copy codec parameters" );
			return false;
		}
		m_codecCtx->thread_count = 0;
		m_codecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

		ret = avcodec_open2( m_codecCtx, swCodec, nullptr );
		if ( ret < 0 ) {
			av_strerror( ret, m_errbuf, sizeof( m_errbuf ) );
			return false;
		}
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
	if ( !DecodeNextFrame() )
	{
		if ( m_bHardwareDecode )
		{
			// HW decode failed (e.g. unsupported surface format) - retry as software.
			Warning( "[video] Hardware decode failed on initial frame (%s), retrying with software decoder\n", m_errbuf );
			if ( !RestartSoftwareDecode( swCodec ) )
				return false;
		}
		else
		{
			snprintf( m_errbuf, sizeof( m_errbuf ), "Failed to decode initial frame" );
			return false;
		}
	}
	m_videoTimeLastFrame = 0.0;

	PrintInfo( filePath );

	// -------------------------------------------------------------------
	// Decide the output path from the ACTUAL first frame format:
	//   DXVA2_VLD -> hardware path (VideoProcessBlt into the RGB RT)
	//   YUV       -> existing software path
	// -------------------------------------------------------------------
	if ( m_bHardwareDecode && m_frame && m_frame->format == AV_PIX_FMT_DXVA2_VLD )
	{
		// Hardware path: the decoder surface stays on the GPU. Report the
		// actual surface format and initialize the RGB material.
		AVHWFramesContext *pHwFramesCtx = m_frame->hw_frames_ctx
			? reinterpret_cast< AVHWFramesContext * >( m_frame->hw_frames_ctx->data )
			: nullptr;

		if ( !pHwFramesCtx )
		{
			Warning( "[video] DXVA2 frame has no AVHWFramesContext - falling back to software path\n" );

			if ( !RestartSoftwareDecode( swCodec ) )
				return false;
		}
		else
		{
			const AVPixelFormat swFormat = pHwFramesCtx->sw_format;

			bool bSupportedFormat = false;

			switch ( swFormat )
			{
				case AV_PIX_FMT_NV12:
				case AV_PIX_FMT_P010LE:
				case AV_PIX_FMT_P016LE:
					bSupportedFormat = true;
					break;

				default:
					break;
			}

			if ( !bSupportedFormat )
			{
				Warning(
					"[video] DXVA2: unsupported software format %s - "
					"falling back to software path\n",
					av_get_pix_fmt_name( swFormat ) );

				if ( !RestartSoftwareDecode( swCodec ) )
					return false;
			}
			else
			{
				const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get( swFormat );

				if ( !desc )
				{
					Warning(
						"[video] DXVA2: unknown software format %d - "
						"falling back to software path\n",
						static_cast< int >( swFormat ) );

					if ( !RestartSoftwareDecode( swCodec ) )
						return false;
				}
				else
				{

					m_hwSwFormat = static_cast< int >( swFormat );

					const int bitDepth = desc->comp[ 0 ].depth;
					const int colorRange     = m_frame->color_range;
					const int colorSpace     = m_frame->colorspace;
					const int colorTransferC = m_frame->color_trc;
					const int colorPrimaries = m_frame->color_primaries;

					IDirect3DDevice9 *pDevice = RD_D3D9::GetDevice();

					if ( !pDevice || !m_devMgr
						|| !m_videoSEMaterial.InitHW(
							pDevice,
							m_devMgr,
							SwFormatToD3DFormat( swFormat ),
							bitDepth,
							m_frame->width,
							m_frame->height,
							colorRange,
							colorSpace,
							colorTransferC,
							colorPrimaries ) )
					{
						Warning(
							"[video] DXVA2 hardware material init failed - "
							"falling back to software path\n" );

						if ( !RestartSoftwareDecode( swCodec ) )
							return false;
					}
					else if ( !m_videoSEMaterial.UpdateHW(
						reinterpret_cast< IDirect3DSurface9 * >( m_frame->data[ 3 ] ) ) )
					{
						Warning(
							"[video] DXVA2 VideoProcessBlt failed on the first frame - "
							"falling back to software path\n" );

						m_videoSEMaterial.Reset();

						if ( !RestartSoftwareDecode( swCodec ) )
							return false;
					}
					else
					{
						Pause();
						m_isValid = true;
						return true;
					}
				}
			}
		}
	}

	// Software path: hardware was unavailable, or the hwaccel did not attach
	// (e.g. VP8 has no DXVA2 hwaccel) - drop the HW state and validate the
	// decoded YUV frame as before.
	if ( m_bHardwareDecode )
	{
		Warning( "[video] HW decode active but first frame is not a DXVA2 surface (pix_fmt=%s) - falling back to software path\n", av_get_pix_fmt_name( static_cast< AVPixelFormat >( m_frame->format ) ) );
		if ( !RestartSoftwareDecode( swCodec ) )
			return false;
	}

	const AVPixelFormat pixFmt = static_cast< AVPixelFormat >( m_frame->format );
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get( pixFmt );

	// Validate pixel format and frame dimensions for expected planar YUV 3-component format
	if ( !desc
		|| !( desc->flags & AV_PIX_FMT_FLAG_PLANAR )
		|| desc->nb_components != 3
		|| strncmp( desc->name, "yuv", 3 ) != 0 )
	{
		snprintf( m_errbuf, sizeof( m_errbuf ), "Unsupported pixel format or dimensions: %s", av_get_pix_fmt_name( pixFmt ) );
		return false;
	}

	const int bitDepth = desc->comp[ 0 ].depth;
	if ( ( bitDepth == 8 && m_frame->width != m_frame->linesize[ 0 ] )
		|| ( bitDepth > 8 && m_frame->width != ( m_frame->linesize[ 0 ] >> 1 ) ) )
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
	if ( m_frame )
		av_frame_unref( m_frame );   // drop any stale frame (may hold a DXVA2 surface)

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

	// After a device reset, restore the hardware resources before decoding.
	if ( m_bHWReinitPending && !RecreateHardwareDecode() )
	{
		m_isValid = false;
		return false;
	}

	if ( !m_isPlaying ) return true;

	// Compute current video time including paused duration
	m_videoTimeCurrent = ( Plat_FloatTime() - m_videoTimeStart ) + m_videoTimePaused;

	// Decode next frame only if next frame timestamp >= current time
	if ( m_videoTimeLastFrame <= m_videoTimeCurrent && DecodeNextFrame() ) {
		if ( m_bHardwareDecode && m_frame && m_frame->format == AV_PIX_FMT_DXVA2_VLD )
			m_videoSEMaterial.UpdateHW( reinterpret_cast< IDirect3DSurface9 * >( m_frame->data[ 3 ] ) );
		else
		{
			if ( m_bHardwareDecode )
				Warning( "[video] HW decode active but frame is not a DXVA2 surface (pix_fmt=%s) - using software upload\n", av_get_pix_fmt_name( static_cast< AVPixelFormat >( m_frame->format ) ) );
			m_videoSEMaterial.Update();
		}
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
// Hardware (DXVA2) decode path
// -------------------------------------------------------------------

void VideoFFmpegPlayer::DeviceResetCallback( bool bPreReset )
{
	if ( s_pHWResetOwner )
		s_pHWResetOwner->HandleDeviceReset( bPreReset );
}

void VideoFFmpegPlayer::HandleDeviceReset( bool bPreReset )
{
	if ( !m_bHardwareDecode )
		return;

	if ( bPreReset )
	{
		// Release everything D3D9 BEFORE the device tears down its resources.
		m_videoSEMaterial.ReleaseHWResources();
		if ( m_codecCtx )
			av_buffer_unref( &m_codecCtx->hw_frames_ctx );
		av_buffer_unref( &m_hwFramesCtx );
		m_bHWReinitPending = true;
	}
	else
	{
		// Rebind the DXVA2 device manager to the (recreated) game device.
		// The reset token is unchanged - ResetDevice() takes it by value.
		IDirect3DDevice9 *pDevice = RD_D3D9::GetDevice();
		if ( m_devMgr && pDevice )
			m_devMgr->ResetDevice( pDevice, m_devMgrResetToken );
		m_bHWReinitPending = true; // full recreation happens in Update()
	}
}

bool VideoFFmpegPlayer::InitHardwareDecode() noexcept
{
#ifdef _WIN32
	if ( !rd_video_hwdec.GetBool() )
	{
		Warning( "[video] DXVA2 hardware decode disabled by rd_video_hwdec\n" );
		return false;
	}

	IDirect3DDevice9 *pDevice = RD_D3D9::FindDevice();
	if ( !pDevice )
	{
		Warning( "[video] DXVA2 hardware decode unavailable: no D3D9 device found\n" );
		return false;
	}

	// Only codecs with a DXVA2 hwaccel in FFmpeg are attempted (VP8 has none).
	switch ( m_codecCtx->codec_id )
	{
		case AV_CODEC_ID_AV1:
		case AV_CODEC_ID_VP9:
			break;
		default:
			Warning( "[video] DXVA2 hardware decode unavailable: codec %s has no DXVA2 hwaccel\n", avcodec_get_name( m_codecCtx->codec_id ) );
			return false;
	}

	// DXVA2 device manager on the game device.
	UINT resetToken = 0;
	IDirect3DDeviceManager9 *pDevMgr = nullptr;
	HRESULT hr = DXVA2CreateDirect3DDeviceManager9( &resetToken, &pDevMgr );
	if ( FAILED( hr ) || !pDevMgr )
	{
		Warning( "[video] DXVA2 hardware decode unavailable: DXVA2CreateDirect3DDeviceManager9 failed (0x%08X)\n", ( unsigned int )hr );
		return false;
	}
	hr = pDevMgr->ResetDevice( pDevice, resetToken );
	if ( FAILED( hr ) )
	{
		Warning( "[video] DXVA2 hardware decode unavailable: device manager ResetDevice failed (0x%08X)\n", ( unsigned int )hr );
		pDevMgr->Release();
		return false;
	}
	m_devMgr = pDevMgr;
	m_devMgrResetToken = resetToken;

	// FFmpeg AVHWDeviceContext (DXVA2) bound to the game device manager.
	m_hwDeviceCtx = av_hwdevice_ctx_alloc( AV_HWDEVICE_TYPE_DXVA2 );
	if ( !m_hwDeviceCtx )
	{
		Warning( "[video] DXVA2 hardware decode unavailable: failed to allocate AVHWDeviceContext\n" );
		ShutdownHardwareDecode();
		return false;
	}
	AVHWDeviceContext *pDeviceCtx = reinterpret_cast< AVHWDeviceContext * >( m_hwDeviceCtx->data );
	AVDXVA2DeviceContext *pDxva2Ctx = reinterpret_cast< AVDXVA2DeviceContext * >( pDeviceCtx->hwctx );
	pDxva2Ctx->devmgr = pDevMgr; // av_hwdevice_ctx_init() takes its own reference
	if ( av_hwdevice_ctx_init( m_hwDeviceCtx ) < 0 )
	{
		Warning( "[video] DXVA2 hardware decode unavailable: av_hwdevice_ctx_init failed\n" );
		ShutdownHardwareDecode();
		return false;
	}

	// Pick the decoder surface format from the stream's coded format
	// (P010 for 10-bit content, NV12 otherwise).
	AVPixelFormat swFormat = AV_PIX_FMT_NV12;
	switch ( m_videoStream->codecpar->format )
	{
		case AV_PIX_FMT_YUV420P10LE:
		case AV_PIX_FMT_YUV422P10LE:
			swFormat = AV_PIX_FMT_P010;
			break;
		case AV_PIX_FMT_YUV420P12LE:
			// DXVA2 D3D9 surfaces are NV12/P010 in practice; a 12-bit source is
			// mapped onto the 10-bit P010 surface and loses precision here.
			Warning( "[video] DXVA2: 12-bit source mapped to 10-bit P010 surface (precision loss)\n" );
			swFormat = AV_PIX_FMT_P010;
			break;
		default:
			swFormat = AV_PIX_FMT_NV12;
			break;
	}

	AVBufferRef *framesRef = av_hwframe_ctx_alloc( m_hwDeviceCtx );
	if ( !framesRef )
	{
		Warning( "[video] DXVA2 hardware decode unavailable: failed to allocate AVHWFramesContext\n" );
		ShutdownHardwareDecode();
		return false;
	}
	AVHWFramesContext *pHwFramesCtx = reinterpret_cast< AVHWFramesContext * >( framesRef->data );
	pHwFramesCtx->format = AV_PIX_FMT_DXVA2_VLD;
	pHwFramesCtx->sw_format = swFormat;
	pHwFramesCtx->width = m_codecCtx->width;
	pHwFramesCtx->height = m_codecCtx->height;
	if ( av_hwframe_ctx_init( framesRef ) < 0 )
	{
		Warning( "[video] DXVA2 hardware decode unavailable: av_hwframe_ctx_init failed (unsupported surface format?)\n" );
		av_buffer_unref( &framesRef );
		ShutdownHardwareDecode();
		return false;
	}
	m_hwFramesCtx = framesRef;

	// Hardware-acceleration model: open the REGULAR software decoder with
	// hw_device_ctx + hw_frames_ctx set - FFmpeg attaches the DXVA2 hwaccel
	// automatically (the same mechanism "-hwaccel dxva2" uses).
	m_codecCtx->hw_device_ctx = av_buffer_ref( m_hwDeviceCtx );
	m_codecCtx->hw_frames_ctx = av_buffer_ref( framesRef );

	// DXVA2 hardware decoding is incompatible with frame threading - force a
	// single-threaded decode so the hwaccel actually attaches.
	m_codecCtx->thread_count = 1;
	m_codecCtx->thread_type = 0;

	if ( avcodec_open2( m_codecCtx, m_swCodec, nullptr ) < 0 )
	{
		// Leave the context closed; Init() opens it as pure software.
		Warning( "[video] DXVA2 hardware decode unavailable: decoder failed to open with hwaccel attached\n" );
		ShutdownHardwareDecode();
		return false;
	}

	m_hwSwFormat = static_cast< int >( swFormat );
	return true;
#endif
	return false;
}

void VideoFFmpegPlayer::ShutdownHardwareDecode() noexcept
{
	if ( s_pHWResetOwner == this )
		s_pHWResetOwner = nullptr;

	m_bHardwareDecode = false;
	m_bHWReinitPending = false;

	if ( m_codecCtx )
	{
		av_buffer_unref( &m_codecCtx->hw_frames_ctx );
		av_buffer_unref( &m_codecCtx->hw_device_ctx );
	}
	av_buffer_unref( &m_hwFramesCtx );
	av_buffer_unref( &m_hwDeviceCtx );

	if ( m_devMgr )
	{
		m_devMgr->Release();
		m_devMgr = nullptr;
	}
	m_hwSwFormat = -1;
}

/**
 * @brief Drop the DXVA2 hardware path and restart decoding as software.
 *
 * Frees the single-threaded HW decoder while the device manager is still
 * alive, re-opens a multithreaded software decoder, rewinds the demuxer to
 * the keyframe and decodes the first frame (the first decode attempt always
 * consumed packets mid-stream, so a fresh decoder must not start there).
 *
 * @param codec Software decoder to open.
 * @return true on success, false on failure (GetError() has details).
 */
bool VideoFFmpegPlayer::RestartSoftwareDecode( const AVCodec *codec ) noexcept
{
	// Drop the decoded frame FIRST - it may still hold a DXVA2 surface whose
	// COM objects must be released while the decoder/device manager are alive.
	av_frame_unref( m_frame );

	// Keep the codec reference consistent with the (re)opened software decoder.
	m_swCodec = codec;

	// Tear down the decoder (and any attached hwaccel) while the DXVA2
	// device manager is still alive, then release the remaining HW state.
	avcodec_free_context( &m_codecCtx );
	ShutdownHardwareDecode();

	m_codecCtx = avcodec_alloc_context3( codec );
	if ( !m_codecCtx ) {
		snprintf( m_errbuf, sizeof( m_errbuf ), "Failed to allocate codec context" );
		return false;
	}
	if ( avcodec_parameters_to_context( m_codecCtx, m_videoStream->codecpar ) < 0 ) {
		snprintf( m_errbuf, sizeof( m_errbuf ), "Failed to copy codec parameters" );
		return false;
	}
	m_codecCtx->thread_count = 0;
	m_codecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	if ( avcodec_open2( m_codecCtx, codec, nullptr ) < 0 ) {
		snprintf( m_errbuf, sizeof( m_errbuf ), "Failed to open software decoder" );
		return false;
	}

	// The first decode attempt consumed packets mid-stream; rewind to the
	// keyframe so the fresh software decoder can start cleanly.
	avcodec_flush_buffers( m_codecCtx );
	if ( av_seek_frame( m_fmtCtx, m_streamIndex, 0, AVSEEK_FLAG_BACKWARD ) < 0 ) {
		snprintf( m_errbuf, sizeof( m_errbuf ), "Failed to rewind stream for software decode" );
		return false;
	}
	if ( !DecodeNextFrame() ) {
		snprintf( m_errbuf, sizeof( m_errbuf ), "Failed to decode initial frame" );
		return false;
	}
	return true;
}

bool VideoFFmpegPlayer::RecreateHardwareDecode() noexcept
{
#ifdef _WIN32
	m_bHWReinitPending = false;
	if ( !m_bHardwareDecode )
		return true;

	IDirect3DDevice9 *pDevice = RD_D3D9::GetDevice();
	if ( !m_devMgr || !pDevice || !m_hwDeviceCtx || !m_swCodec || !m_videoStream )
		return false;

	// 1. Recreate the hw frames context (new decoder surfaces).
	AVBufferRef *framesRef = av_hwframe_ctx_alloc( m_hwDeviceCtx );
	if ( !framesRef )
		return false;
	AVHWFramesContext *pHwFramesCtx = reinterpret_cast< AVHWFramesContext * >( framesRef->data );
	pHwFramesCtx->format = AV_PIX_FMT_DXVA2_VLD;
	pHwFramesCtx->sw_format = static_cast< AVPixelFormat >( m_hwSwFormat );
	pHwFramesCtx->width = m_codecCtx->width;
	pHwFramesCtx->height = m_codecCtx->height;
	if ( av_hwframe_ctx_init( framesRef ) < 0 )
	{
		av_buffer_unref( &framesRef );
		return false;
	}

	// 2. Reopen the decoder (old surfaces are dead after the device reset).
	//    Build into a temporary context and commit only on full success, so a
	//    failure never leaves the player with a half-open decoder.
	AVCodecContext *pNewCodecCtx = avcodec_alloc_context3( m_swCodec );
	if ( !pNewCodecCtx )
	{
		av_buffer_unref( &framesRef );
		return false;
	}
	avcodec_parameters_to_context( pNewCodecCtx, m_videoStream->codecpar );
	pNewCodecCtx->thread_count = 1;
	pNewCodecCtx->thread_type = 0;
	pNewCodecCtx->hw_device_ctx = av_buffer_ref( m_hwDeviceCtx );
	pNewCodecCtx->hw_frames_ctx = av_buffer_ref( framesRef );

	if ( avcodec_open2( pNewCodecCtx, m_swCodec, nullptr ) < 0 )
	{
		avcodec_free_context( &pNewCodecCtx );
		av_buffer_unref( &framesRef );
		return false;
	}

	// 3. Recreate the material hardware resources. The native RT surface is
	//    re-acquired through the SetRenderTarget hook (never the old pointer).
	if ( !m_videoSEMaterial.RecreateHWResources( pDevice, m_devMgr ) )
	{
		avcodec_free_context( &pNewCodecCtx );
		av_buffer_unref( &framesRef );
		return false;
	}

	avcodec_free_context( &m_codecCtx );
	m_codecCtx = pNewCodecCtx;
	av_buffer_unref( &m_hwFramesCtx );
	m_hwFramesCtx = framesRef;
	avcodec_flush_buffers( m_codecCtx );
	return true;
#else
	return false;
#endif
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
	ConColorMsg( Color( 153, 204, 255, 255 ), "    Frame Rate:      %.2f fps\n", ( fps.den != 0 ) ? static_cast< double >( fps.num ) / fps.den : 0.0 );
	ConColorMsg( Color( 153, 204, 255, 150 ), "    Duration:        %.2f s\n", dur );
	if ( desc != nullptr ) {
		ConColorMsg( Color( 153, 204, 255, 255 ), "    Encoder Name:     %s (%s)\n", desc->name, desc->long_name );
	}
	else {
		ConColorMsg( Color( 153, 204, 255, 255 ), "    Encoder Name:     unknown\n" );
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
		ConColorMsg( Color( 153, 204, 255, 255 ), "    Decoder Name:     %s (%s)\n", decoder->name ? decoder->name : "unknown", decoder->long_name ? decoder->long_name : "unknown" );
	}
	else {
		ConColorMsg( Color( 153, 204, 255, 255 ), "    Decoder Name:     unknown\n" );
	}
	ConColorMsg( Color( 153, 204, 255, 150 ), "    Pixel Format:    %s\n", pixFmt ? pixFmt : "unknown" );
	ConColorMsg( Color( 153, 204, 255, 255 ), "    Color Range:     %s\n", range_str ? range_str : "unknown" );
	ConColorMsg( Color( 153, 204, 255, 150 ), "    Color Space:     %s\n", space_str ? space_str : "unknown" );
	ConColorMsg( Color( 153, 204, 255, 255 ), "    Color Primaries: %s\n", primaries_str ? primaries_str : "unknown" );
	ConColorMsg( Color( 153, 204, 255, 150 ), "    Color Transfer:  %s\n", transfer_str ? transfer_str : "unknown" );
}
