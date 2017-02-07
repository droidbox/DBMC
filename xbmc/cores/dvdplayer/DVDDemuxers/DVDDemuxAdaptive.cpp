/*
 *      Copyright (C) 2016 Christian Browet
 *      Copyright (C) 2016-2016 peak3d
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "DVDDemuxAdaptive.h"

#include "DVDDemuxPacket.h"
#include "DVDDemuxUtils.h"
#include "DVDInputStreams/DVDInputStream.h"

#include "adaptive/DASHByteStream.h"

#ifdef TARGET_ANDROID
#include "android/jni/SystemProperties.h"
#endif
#ifdef TARGET_WINDOWS
#pragma comment(lib, "libexpat.lib")
#pragma comment(lib, "ap4.lib")
#endif

#include "utils/StringUtils.h"
#include "utils/log.h"

CDVDDemuxAdaptive::CDVDDemuxAdaptive()
  : CDVDDemux()
{
  CLog::Log(LOGDEBUG, "CDVDDemuxAdaptive::%s", __FUNCTION__);
}

CDVDDemuxAdaptive::~CDVDDemuxAdaptive()
{
  CLog::Log(LOGDEBUG, "CDVDDemuxAdaptive::%s", __FUNCTION__);
}

bool CDVDDemuxAdaptive::Open(CDVDInputStream* pInput, uint32_t maxWidth, uint32_t maxHeight)
{
  CLog::Log(LOGINFO, "CDVDDemuxAdaptive - matching against %d x %d", maxWidth, maxHeight);
  
  CDASHSession::MANIFEST_TYPE type = CDASHSession::MANIFEST_TYPE_UNKNOWN;
  
  if (pInput->GetFileItem().GetMimeType() == "video/vnd.mpeg.dash.mpd" || pInput->GetFileItem().IsType(".mpd"))  //MPD
    type = CDASHSession::MANIFEST_TYPE_MPD;
  else if (pInput->GetFileItem().GetMimeType() == "application/vnd.ms-sstr+xml" || pInput->GetFileItem().IsType(".ismc") || pInput->GetFileItem().IsType(".ism"))  //ISM
    type = CDASHSession::MANIFEST_TYPE_ISM;
  
  if (type == CDASHSession::MANIFEST_TYPE_UNKNOWN)
    return false;
  
  m_MPDsession.reset(new CDASHSession(type, pInput->GetFileName(), maxWidth, maxHeight, "", "", "special://profile/"));

  if (!m_MPDsession->initialize())
  {
    m_MPDsession = nullptr;
    return false;
  }
  return true;
}

void CDVDDemuxAdaptive::Dispose()
{
}

void CDVDDemuxAdaptive::Reset()
{
}

void CDVDDemuxAdaptive::Abort()
{
}

void CDVDDemuxAdaptive::Flush()
{
}

DemuxPacket*CDVDDemuxAdaptive::Read()
{
  if (!m_MPDsession)
    return NULL;

  CDASHFragmentedSampleReader *sr(m_MPDsession->GetNextSample());

  if (m_MPDsession->CheckChange())
  {
    DemuxPacket *p = CDVDDemuxUtils::AllocateDemuxPacket(0);
    p->iStreamId = DMX_SPECIALID_STREAMCHANGE;
    CLog::Log(LOGDEBUG, "DMX_SPECIALID_STREAMCHANGE");
    return p;
  }

  if (sr)
  {
    DemuxPacket *p = CDVDDemuxUtils::AllocateDemuxPacket(sr->GetSampleDataSize());
    p->dts = sr->DTS() * 1000000;
    p->pts = sr->PTS() * 1000000;
    p->duration = sr->GetDuration() * 1000000;
    p->iStreamId = sr->GetStreamId();
    p->iGroupId = 0;
    p->iSize = sr->GetSampleDataSize();
    memcpy(p->pData, sr->GetSampleData(), p->iSize);

    //CLog::Log(LOGDEBUG, "DTS: %0.4f, PTS:%0.4f, ID: %u SZ: %d", p->dts, p->pts, p->iStreamId, p->iSize);

    sr->ReadSample();
    return p;
  }
  return NULL;
}

bool CDVDDemuxAdaptive::SeekTime(int time, bool backwards, double* startpts)
{
  if (!m_MPDsession)
    return false;

  return m_MPDsession->SeekTime(static_cast<double>(time)*0.001f, 0, !backwards);
}

void CDVDDemuxAdaptive::SetSpeed(int speed)
{
}

int CDVDDemuxAdaptive::GetNrOfStreams()
{
  int n = 0;
  if (m_MPDsession)
    n = m_MPDsession->GetStreamCount();

  return n;
}

CDemuxStream* CDVDDemuxAdaptive::GetStream(int streamid)
{
  CDASHSession::STREAM *stream(m_MPDsession->GetStream(streamid));
  if (!stream)
  {
    CLog::Log(LOGERROR, "CDVDDemuxAdaptive::GetStream(%d): error getting stream", streamid);
    return nullptr;
  }

  return stream->dmuxstrm;
}

void CDVDDemuxAdaptive::EnableStream(int streamid, bool enable)
{
  CLog::Log(LOGDEBUG, "EnableStream(%d: %s)", streamid, enable?"true":"false");

  if (!m_MPDsession)
    return;

  CDASHSession::STREAM *stream(m_MPDsession->GetStream(streamid));
  if (!stream)
    return;

  if (enable)
  {
    if (stream->enabled)
      return;

    stream->enabled = true;

    stream->stream_.start_stream(~0, m_MPDsession->GetWidth(), m_MPDsession->GetHeight());
    const adaptive::AdaptiveTree::Representation *rep(stream->stream_.getRepresentation());
    CLog::Log(LOGDEBUG, "Selecting stream with conditions: w: %u, h: %u, bw: %u",
      stream->stream_.getWidth(), stream->stream_.getHeight(), stream->stream_.getBandwidth());

    if (!stream->stream_.select_stream(true, false, stream->dmuxstrm->iPhysicalId >> 16))
    {
      CLog::Log(LOGERROR, "Unable to select stream!");
      return stream->disable();
    }

    if(rep != stream->stream_.getRepresentation())
    {
      m_MPDsession->UpdateStream(*stream);
      m_MPDsession->CheckChange(true);
    }

    stream->input_ = new CDASHByteStream(&stream->stream_);
    stream->input_file_ = new AP4_File(*stream->input_, AP4_DefaultAtomFactory::Instance_, true);
    AP4_Movie* movie = stream->input_file_->GetMovie();
    if (movie == NULL)
    {
      CLog::Log(LOGERROR, "No MOOV in stream!");
      return stream->disable();
    }

    static const AP4_Track::Type TIDC[adaptive::AdaptiveTree::STREAM_TYPE_COUNT] =
    { AP4_Track::TYPE_UNKNOWN, AP4_Track::TYPE_VIDEO, AP4_Track::TYPE_AUDIO, AP4_Track::TYPE_TEXT };

    AP4_Track *track = movie->GetTrack(TIDC[stream->stream_.get_type()]);
    if (!track)
    {
      CLog::Log(LOGERROR, "No suitable track found in stream");
      return stream->disable();
    }

    stream->reader_ = new CDASHFragmentedSampleReader(stream->input_, movie, track, streamid, m_MPDsession->GetSingleSampleDecryptor(), m_MPDsession->GetPresentationTimeOffset());
    stream->reader_->SetObserver(dynamic_cast<IDASHFragmentObserver*>(m_MPDsession.get()));

    if (!stream->dmuxstrm->ExtraSize)
    {
      // ExtraData is now available......
      stream->dmuxstrm->ExtraSize = stream->reader_->GetExtraDataSize();

      // Set the session Changed to force new GetStreamInfo call from kodi -> addon
      if (stream->dmuxstrm->ExtraSize)
      {
        stream->dmuxstrm->ExtraData = (uint8_t*)malloc(stream->dmuxstrm->ExtraSize);
        memcpy((void*)stream->dmuxstrm->ExtraData, stream->reader_->GetExtraData(), stream->dmuxstrm->ExtraSize);
        m_MPDsession->CheckChange(true);
      }
    }
    return;
  }
  CLog::Log(LOGDEBUG, ">>>> ERROR");
  return stream->disable();
}

int CDVDDemuxAdaptive::GetStreamLength()
{
  if (!m_MPDsession)
    return 0;

  return static_cast<int>(m_MPDsession->GetTotalTime()*1000);
}

std::string CDVDDemuxAdaptive::GetFileName()
{
  if (!m_MPDsession)
    return "";

  return m_MPDsession->GetMpdUrl();
}

void CDVDDemuxAdaptive::GetStreamCodecName(int iStreamId, std::string& strName)
{
  strName = "";

  CDASHSession::STREAM *stream(m_MPDsession->GetStream(iStreamId));
  if (stream)
    strName = stream->codecName;
}
