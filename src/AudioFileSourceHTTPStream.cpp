/*
  AudioFileSourceHTTPStream
  Streaming HTTP source

  Copyright (C) 2017  Earle F. Philhower, III

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#if defined(ESP32) || defined(ESP8266)

#include "AudioFileSourceHTTPStream.h"

AudioFileSourceHTTPStream::AudioFileSourceHTTPStream()
{
  pos = 0;
  reconnectTries = 0;
  saveURL[0] = 0;
  next_chunk = 0;
}

AudioFileSourceHTTPStream::AudioFileSourceHTTPStream(const char *url)
{
  saveURL[0] = 0;
  reconnectTries = 0;
  next_chunk = 0;
  open(url);

}

bool AudioFileSourceHTTPStream::verifyCrlf()
{
  
  uint8_t crlf[3];
  
  client.read(crlf, 2);
  crlf[2] = 0;
  
  if (crlf[0] == '\r' && crlf[1] == '\n') {
    return 1;
  } else {
    return 0;
  }
}

int AudioFileSourceHTTPStream::getChunkSize()
{
  char length[7]; // 6-bit hex string, max value of 16777215, extremely large for a chunk size
  for (int i = 0; i < 6; i++) {
    length[i] = client.read();
    if (length[i] == '\r') {
      length[i] = '\0';
      client.read(); // Discard '\n'
      break;
    }
  }
  unsigned int val = 0;
  auto ret = sscanf(length, "%x", &val);
  if(ret)
  {
    return val;
  }
  else
  {
    return -1;
  }
}

bool AudioFileSourceHTTPStream::open(const char *url)
{
  pos = 0;
  http.begin(client, url);
  http.setReuse(true);
#ifndef ESP32
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
#endif
  const char* headers[] = { "Transfer-Encoding" };
  http.collectHeaders( headers, 1 );
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    cb.st(STATUS_HTTPFAIL, PSTR("Can't open HTTP request"));
    return false;
  }
  if (http.hasHeader("Transfer-Encoding")) {
    audioLogger->printf_P(PSTR("Transfer-Encoding: %s\n"), http.header("Transfer-Encoding").c_str());
    if(http.header("Transfer-Encoding") == String(PSTR("chunked"))) {
      
      next_chunk = getChunkSize();
      if(-1 == next_chunk) 
      {
        return false;
      }
      is_chunked = true;
    } else {
      is_chunked = false;
    }

  } else {
    audioLogger->printf_P(PSTR("No Transfer-Encoding\n"));
    is_chunked = false;
  }
 

  size = http.getSize();
  strncpy(saveURL, url, sizeof(saveURL));
  saveURL[sizeof(saveURL)-1] = 0;
  return true;
}

AudioFileSourceHTTPStream::~AudioFileSourceHTTPStream()
{
  http.end();
}

uint32_t AudioFileSourceHTTPStream::readChunked(void *data, uint32_t len, bool nonBlock)
{
  uint32_t remain = len;
  uint32_t bytesRead = 0;
  uint32_t pos = 0;
  
  while (remain > 0)
  {
    if (remain >= next_chunk)
    {
      while (next_chunk)
      {
        bytesRead = readInternal(data + pos, next_chunk, nonBlock);
        next_chunk -= bytesRead;
        pos += bytesRead;
      }
      remain = len - pos;
      if(!verifyCrlf())
      {
        audioLogger->printf(PSTR("Couldn't read CRLF after chunk, something is wrong !!\n"));
        return 0;
      }
      next_chunk = getChunkSize();
    }
    else
    {
      bytesRead = readInternal(data + pos, remain, nonBlock);
      next_chunk -= bytesRead;
      remain -= bytesRead;
      pos += bytesRead;
    }
  }
  
  return pos;
}

uint32_t AudioFileSourceHTTPStream::read(void *data, uint32_t len)
{
  if (data==NULL) {
    audioLogger->printf_P(PSTR("ERROR! AudioFileSourceHTTPStream::read passed NULL data\n"));
    return 0;
  }
  
  return is_chunked ? readChunked(data, len, false) : readInternal(data, len, false);

}

uint32_t AudioFileSourceHTTPStream::readNonBlock(void *data, uint32_t len)
{
  if (data==NULL) {
    audioLogger->printf_P(PSTR("ERROR! AudioFileSourceHTTPStream::readNonBlock passed NULL data\n"));
    return 0;
  }
  return is_chunked ? readChunked(data, len, true) : readInternal(data, len, true);

}

uint32_t AudioFileSourceHTTPStream::readInternal(void *data, uint32_t len, bool nonBlock)
{
retry:
  if (!http.connected()) {
    cb.st(STATUS_DISCONNECTED, PSTR("Stream disconnected"));
    http.end();
    for (int i = 0; i < reconnectTries; i++) {
      char buff[64];
      sprintf_P(buff, PSTR("Attempting to reconnect, try %d"), i);
      cb.st(STATUS_RECONNECTING, buff);
      delay(reconnectDelayMs);
      if (open(saveURL)) {
        cb.st(STATUS_RECONNECTED, PSTR("Stream reconnected"));
        break;
      }
    }
    if (!http.connected()) {
      cb.st(STATUS_DISCONNECTED, PSTR("Unable to reconnect"));
      return 0;
    }
  }
  if ((size > 0) && (pos >= size)) return 0;

  WiFiClient *stream = http.getStreamPtr();

  // Can't read past EOF...
  if ( (size > 0) && (len > (uint32_t)(pos - size)) ) len = pos - size;

  if (!nonBlock) {
    int start = millis();
    while ((stream->available() < (int)len) && (millis() - start < 500)) yield();
  }

  size_t avail = stream->available();
  if (!nonBlock && !avail) {
    cb.st(STATUS_NODATA, PSTR("No stream data available"));
    http.end();
    goto retry;
  }
  if (avail == 0) return 0;
  if (avail < len) len = avail;
  
  int read = stream->read(reinterpret_cast<uint8_t*>(data), len);
  pos += read;
  return read;
}

bool AudioFileSourceHTTPStream::seek(int32_t pos, int dir)
{
  audioLogger->printf_P(PSTR("ERROR! AudioFileSourceHTTPStream::seek not implemented!"));
  (void) pos;
  (void) dir;
  return false;
}

bool AudioFileSourceHTTPStream::close()
{
  http.end();
  return true;
}

bool AudioFileSourceHTTPStream::isOpen()
{
  return http.connected();
}

uint32_t AudioFileSourceHTTPStream::getSize()
{
  return size;
}

uint32_t AudioFileSourceHTTPStream::getPos()
{
  return pos;
}

#endif
