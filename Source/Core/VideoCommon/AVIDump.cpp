// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#if defined(__FreeBSD__)
#define __STDC_CONSTANT_MACROS 1
#endif

#include <string>
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "Core/ConfigManager.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"  //for TargetRefreshRate
#include "Core/Movie.h"

#include "VideoCommon/AVIDump.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoConfig.h"

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 28, 1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

static AVFormatContext* s_format_context = nullptr;
static AVStream* s_stream = nullptr;
static AVFrame* s_src_frame = nullptr;
static AVFrame* s_scaled_frame = nullptr;
//static AVPixelFormat s_pix_fmt = AV_PIX_FMT_BGR24;
static AVPixelFormat s_pix_fmt = AV_PIX_FMT_YUV420P;
static SwsContext* s_sws_context = nullptr;
static int s_width;
static int s_height;
static u64 s_last_frame;
static bool s_last_frame_is_valid = false;
static bool s_start_dumping = false;
static u64 s_last_pts;
static int s_file_index = 0;
static int s_savestate_index = 0;
static int s_last_savestate_index = 0;

static void InitAVCodec()
{
  static bool first_run = true;
  if (first_run)
  {
    av_register_all();
    avformat_network_init();
    first_run = false;
  }
}

bool AVIDump::Start(int w, int h)
{
  s_pix_fmt = AV_PIX_FMT_RGBA;

  s_width = w;
  s_height = h;

  s_last_frame_is_valid = false;
  s_last_pts = 0;

  InitAVCodec();
  bool success = CreateVideoFile();
  if (!success)
  {
    CloseVideoFile();
    OSD::AddMessage("AVIDump Start failed");
  }
  return success;
}

bool AVIDump::CreateVideoFile()
{
  const char url[] = "rtmp://localhost/live";
  //const char url[] = "framedump.flv";

  //AVOutputFormat* output_format = av_guess_format("rtmp", url, nullptr);
  AVOutputFormat* output_format = av_guess_format("flv", url, nullptr);
  if (!output_format) return false;
  avformat_alloc_output_context2(&s_format_context, output_format, nullptr, url);
  s_format_context->oformat->video_codec = AV_CODEC_ID_H264;
  //avformat_alloc_context();
  
  if (false)
  {
    std::stringstream s_file_index_str;
    s_file_index_str << s_file_index;
    snprintf(s_format_context->filename, sizeof(s_format_context->filename), "%s",
             (File::GetUserPath(D_DUMPFRAMES_IDX) + "framedump" + s_file_index_str.str() + ".flv")
                 .c_str());
    File::CreateFullPath(s_format_context->filename);

    // Ask to delete file
    if (File::Exists(s_format_context->filename))
    {
      if (SConfig::GetInstance().m_DumpFramesSilent ||
          AskYesNoT("Delete the existing file '%s'?", s_format_context->filename))
      {
        File::Delete(s_format_context->filename);
      }
      else
      {
        // Stop and cancel dumping the video
        return false;
      }
    }
  }

  AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  //s_format_context->flags |= AVFMT_GLOBALHEADER;
  
  if (!(s_stream = avformat_new_stream(s_format_context, codec)))
  {
    return false;
  }
  s_stream->id = s_format_context->nb_streams-1;
  
  AVCodecContext* context = s_stream->codec;
  context->flags |= CODEC_FLAG_GLOBAL_HEADER;
  //context->codec_id =
  //    g_Config.bUseFFV1 ? AV_CODEC_ID_FFV1 : s_format_context->oformat->video_codec;
  context->codec_id = AV_CODEC_ID_H264;
  //if (!g_Config.bUseFFV1)
  //  context->codec_tag =
  //      MKTAG('X', 'V', 'I', 'D');  // Force XVID FourCC for better compatibility
  context->codec_type = AVMEDIA_TYPE_VIDEO;
  //context->bit_rate = 400000;
  context->rc_max_rate = 3000000;
  context->rc_buffer_size = 6000000;
  context->width = s_width;
  context->height = s_height;
  context->time_base.num = 1;
  context->time_base.den = VideoInterface::GetTargetRefreshRate();
  context->gop_size = 12;
  context->pix_fmt = g_Config.bUseFFV1 ? AV_PIX_FMT_BGRA : AV_PIX_FMT_YUV420P;
  //av_opt_set(context->priv_data, "preset", "veryfast", 0);

  if ((avcodec_open2(context, codec, nullptr) < 0))
  {
    return false;
  }

  s_src_frame = av_frame_alloc();
  s_scaled_frame = av_frame_alloc();

  s_scaled_frame->format = context->pix_fmt;
  s_scaled_frame->width = s_width;
  s_scaled_frame->height = s_height;

#if LIBAVCODEC_VERSION_MAJOR >= 55
  if (av_frame_get_buffer(s_scaled_frame, 1))
    return false;
#else
  if (avcodec_default_get_buffer(context, s_scaled_frame))
    return false;
#endif

  NOTICE_LOG(VIDEO, "Opening file %s for dumping", s_format_context->filename);
  if (avio_open(&s_format_context->pb, s_format_context->filename, AVIO_FLAG_WRITE) < 0 ||
      avformat_write_header(s_format_context, nullptr))
  {
    WARN_LOG(VIDEO, "Could not open %s", s_format_context->filename);
    return false;
  }

  OSD::AddMessage(StringFromFormat("Dumping Frames to \"%s\" (%dx%d)", s_format_context->filename,
                                   s_width, s_height));

  av_dump_format(s_format_context, 0, url, 1);

  return true;
}

static void PreparePacket(AVPacket* pkt)
{
  av_init_packet(pkt);
  pkt->data = nullptr;
  pkt->size = 0;
}

void AVIDump::AddFrame(const u8* data, int width, int height, int stride, const Frame& state)
{
  // Assume that the timing is valid, if the savestate id of the new frame
  // doesn't match the last one.
  if (state.savestate_index != s_last_savestate_index)
  {
    s_last_savestate_index = state.savestate_index;
    s_last_frame_is_valid = false;
  }

  //CheckResolution(width, height);
  s_src_frame->data[0] = const_cast<u8*>(data);
  s_src_frame->linesize[0] = stride;
  s_src_frame->format = s_pix_fmt;
  s_src_frame->width = s_width;
  s_src_frame->height = s_height;

  // Convert image from {BGR24, RGBA} to desired pixel format
  if ((s_sws_context =
           sws_getCachedContext(s_sws_context, width, height, s_pix_fmt, s_width, s_height,
                                s_stream->codec->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr)))
  {
    sws_scale(s_sws_context, s_src_frame->data, s_src_frame->linesize, 0, height,
              s_scaled_frame->data, s_scaled_frame->linesize);
  }

  // Encode and write the image.
  AVPacket pkt;
  PreparePacket(&pkt);
  int got_packet = 0;
  int error = 0;
  u64 delta;
  s64 last_pts;
  // Check to see if the first frame being dumped is the first frame of output from the emulator.
  // This prevents an issue with starting dumping later in emulation from placing the frames
  // incorrectly.
  if (!s_last_frame_is_valid)
  {
    s_last_frame = state.ticks;
    s_last_frame_is_valid = true;
  }
  if (!s_start_dumping && state.first_frame)
  {
    delta = state.ticks;
    last_pts = AV_NOPTS_VALUE;
    s_start_dumping = true;
  }
  else
  {
    delta = state.ticks - s_last_frame;
    last_pts = (s_last_pts * s_stream->codec->time_base.den) / state.ticks_per_second;
  }
  u64 pts_in_ticks = s_last_pts + delta;
  s_scaled_frame->pts = (pts_in_ticks * s_stream->codec->time_base.den) / state.ticks_per_second;
  if (s_scaled_frame->pts != last_pts)
  {
    s_last_frame = state.ticks;
    s_last_pts = pts_in_ticks;
    error = avcodec_encode_video2(s_stream->codec, &pkt, s_scaled_frame, &got_packet);
  }
  if (!error && got_packet)
  {
    // Write the compressed frame in the media file.
    if (pkt.pts != (s64)AV_NOPTS_VALUE)
    {
      pkt.pts = av_rescale_q(pkt.pts, s_stream->codec->time_base, s_stream->time_base);
    }
    if (pkt.dts != (s64)AV_NOPTS_VALUE)
    {
      pkt.dts = av_rescale_q(pkt.dts, s_stream->codec->time_base, s_stream->time_base);
    }
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56, 60, 100)
    if (s_stream->codec->coded_frame->key_frame)
      pkt.flags |= AV_PKT_FLAG_KEY;
#endif
    pkt.stream_index = s_stream->index;
    av_interleaved_write_frame(s_format_context, &pkt);
  }
  if (error)
    ERROR_LOG(VIDEO, "Error while encoding video: %d", error);
}

void AVIDump::Stop()
{
/*
  AVPacket pkt;
  int got_packet = 1;
  int error = 0;

  while (got_packet)
  {
    // Handle delayed frames.
    PreparePacket(&pkt);
    error = avcodec_encode_video2(s_stream->codec, &pkt, nullptr, &got_packet);
    if (error)
    {
      ERROR_LOG(VIDEO, "Error while stopping video: %d", error);
      break;
    }
    
    pkt.stream_index = s_stream->index;
    av_interleaved_write_frame(s_format_context, &pkt);
  }
*/
  av_write_trailer(s_format_context);
  CloseVideoFile();
  s_file_index = 0;
  NOTICE_LOG(VIDEO, "Stopping frame dump");
  OSD::AddMessage("Stopped dumping frames");
  std::cout << "Stopped dumping frames\n";
}

void AVIDump::CloseVideoFile()
{
/*
  if (s_stream)
  {
    if (s_stream->codec)
    {
#if LIBAVCODEC_VERSION_MAJOR < 55
      avcodec_default_release_buffer(s_stream->codec, s_src_frame);
#endif
      avcodec_close(s_stream->codec);
    }
    av_freep(&s_stream);
  }
*/
  av_frame_free(&s_src_frame);
  av_frame_free(&s_scaled_frame);

  if (s_format_context)
  {
    if (s_format_context->pb)
      avio_close(s_format_context->pb);
    avformat_free_context(s_format_context);
  }

  if (s_sws_context)
  {
    sws_freeContext(s_sws_context);
    s_sws_context = nullptr;
  }
}

void AVIDump::DoState()
{
  s_savestate_index++;
}

void AVIDump::CheckResolution(int width, int height)
{
  // We check here to see if the requested width and height have changed since the last frame which
  // was dumped, then create a new file accordingly. However, is it possible for the height
  // (possibly width as well, but no examples known) to have a value of zero. This can occur as the
  // VI is able to be set to a zero value for height/width to disable output. If this is the case,
  // simply keep the last known resolution of the video for the added frame.
  if ((width != s_width || height != s_height) && (width > 0 && height > 0))
  {
    int temp_file_index = s_file_index;
    Stop();
    s_file_index = temp_file_index + 1;
    Start(width, height);
  }
}

AVIDump::Frame AVIDump::FetchState(u64 ticks)
{
  Frame state;
  state.ticks = ticks;
  state.first_frame = Movie::GetCurrentFrame() < 1;
  state.ticks_per_second = SystemTimers::GetTicksPerSecond();
  state.savestate_index = s_savestate_index;
  return state;
}
