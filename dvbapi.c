/*
 * dvbapi.c: Interface to the DVB driver
 *
 * See the main source file 'vdr.c' for copyright information and
 * how to reach the author.
 *
 * DVD support initially written by Andreas Schultz <aschultz@warp10.net>
 * based on dvdplayer-0.5 by Matjaz Thaler <matjaz.thaler@guest.arnes.si>
 *
 * $Id: dvbapi.c 1.101 2001/08/06 16:24:13 kls Exp $
 */

//#define DVDDEBUG        1

#include "dvbapi.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
extern "C" {
#define HAVE_BOOLEAN
#include <jpeglib.h>
}
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef DVDSUPPORT
extern "C" {
#include "ac3dec/ac3.h"
}
#endif //DVDSUPPORT

#include "config.h"
#include "recording.h"
#include "remux.h"
#include "ringbuffer.h"
#include "tools.h"
#include "videodir.h"

#define DEV_VIDEO      "/dev/video"
#define DEV_OST_OSD    "/dev/ost/osd"
#define DEV_OST_QAMFE  "/dev/ost/qamfe"
#define DEV_OST_QPSKFE "/dev/ost/qpskfe"
#define DEV_OST_SEC    "/dev/ost/sec"
#define DEV_OST_DVR    "/dev/ost/dvr"
#define DEV_OST_DEMUX  "/dev/ost/demux"
#define DEV_OST_VIDEO  "/dev/ost/video"
#define DEV_OST_AUDIO  "/dev/ost/audio"

// The size of the array used to buffer video data:
// (must be larger than MINVIDEODATA - see remux.h)
#define VIDEOBUFSIZE    (1024*1024)
#define AC3_BUFFER_SIZE (6*1024*16)

// The maximum size of a single frame:
#define MAXFRAMESIZE (192*1024)

#define FRAMESPERSEC 25

// The maximum file size is limited by the range that can be covered
// with 'int'. 4GB might be possible (if the range is considered
// 'unsigned'), 2GB should be possible (even if the range is considered
// 'signed'), so let's use 1GB for absolute safety (the actual file size
// may be slightly higher because we stop recording only before the next
// 'I' frame, to have a complete Group Of Pictures):
#define MAXVIDEOFILESIZE (1024*1024*1024) // Byte
#define MAXFILESPERRECORDING 255

#define MINFREEDISKSPACE    (512) // MB
#define DISKCHECKINTERVAL   100 // seconds

#define INDEXFILESUFFIX     "/index.vdr"
#define RECORDFILESUFFIX    "/%03d.vdr"
#define RECORDFILESUFFIXLEN 20 // some additional bytes for safety...

// The number of frames to back up when resuming an interrupted replay session:
#define RESUMEBACKUP (10 * FRAMESPERSEC)

// The maximum time we wait before assuming that a recorded video data stream
// is broken:
#define MAXBROKENTIMEOUT 30 // seconds

#define CHECK(s) { if ((s) < 0) LOG_ERROR; } // used for 'ioctl()' calls

typedef unsigned char uchar;

const char *IndexToHMSF(int Index, bool WithFrame)
{
  static char buffer[16];
  int f = (Index % FRAMESPERSEC) + 1;
  int s = (Index / FRAMESPERSEC);
  int m = s / 60 % 60;
  int h = s / 3600;
  s %= 60;
  snprintf(buffer, sizeof(buffer), WithFrame ? "%d:%02d:%02d.%02d" : "%d:%02d:%02d", h, m, s, f);
  return buffer;
}

int HMSFToIndex(const char *HMSF)
{
  int h, m, s, f = 0;
  if (3 <= sscanf(HMSF, "%d:%d:%d.%d", &h, &m, &s, &f))
     return (h * 3600 + m * 60 + s) * FRAMESPERSEC + f - 1;
  return 0;
}

// --- cIndexFile ------------------------------------------------------------

class cIndexFile {
private:
  struct tIndex { int offset; uchar type; uchar number; short reserved; };
  int f;
  char *fileName, *pFileExt;
  int size, last;
  tIndex *index;
  cResumeFile resumeFile;
  bool CatchUp(void);
public:
  cIndexFile(const char *FileName, bool Record);
  ~cIndexFile();
  bool Ok(void) { return index != NULL; }
  void Write(uchar PictureType, uchar FileNumber, int FileOffset);
  bool Get(int Index, uchar *FileNumber, int *FileOffset, uchar *PictureType = NULL, int *Length = NULL);
  int GetNextIFrame(int Index, bool Forward, uchar *FileNumber = NULL, int *FileOffset = NULL, int *Length = NULL);
  int Get(uchar FileNumber, int FileOffset);
  int Last(void) { CatchUp(); return last; }
  int GetResume(void) { return resumeFile.Read(); }
  bool StoreResume(int Index) { return resumeFile.Save(Index); }
  };

cIndexFile::cIndexFile(const char *FileName, bool Record)
:resumeFile(FileName)
{
  f = -1;
  fileName = pFileExt = NULL;
  size = 0;
  last = -1;
  index = NULL;
  if (FileName) {
     fileName = new char[strlen(FileName) + strlen(INDEXFILESUFFIX) + 1];
     if (fileName) {
        strcpy(fileName, FileName);
        pFileExt = fileName + strlen(fileName);
        strcpy(pFileExt, INDEXFILESUFFIX);
        int delta = 0;
        if (access(fileName, R_OK) == 0) {
           struct stat buf;
           if (stat(fileName, &buf) == 0) {
              delta = buf.st_size % sizeof(tIndex);
              if (delta) {
                 delta = sizeof(tIndex) - delta;
                 esyslog(LOG_ERR, "ERROR: invalid file size (%d) in '%s'", buf.st_size, fileName);
                 }
              last = (buf.st_size + delta) / sizeof(tIndex) - 1;
              if (!Record && last >= 0) {
                 size = last + 1;
                 index = new tIndex[size];
                 if (index) {
                    f = open(fileName, O_RDONLY);
                    if (f >= 0) {
                       if ((int)read(f, index, buf.st_size) != buf.st_size) {
                          esyslog(LOG_ERR, "ERROR: can't read from file '%s'", fileName);
                          delete index;
                          index = NULL;
                          close(f);
                          f = -1;
                          }
                       // we don't close f here, see CatchUp()!
                       }
                    else
                       LOG_ERROR_STR(fileName);
                    }
                 else
                    esyslog(LOG_ERR, "ERROR: can't allocate %d bytes for index '%s'", size * sizeof(tIndex), fileName);
                 }
              }
           else
              LOG_ERROR;
           }
        else if (!Record)
           isyslog(LOG_INFO, "missing index file %s", fileName);
        if (Record) {
           if ((f = open(fileName, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) >= 0) {
              if (delta) {
                 esyslog(LOG_ERR, "ERROR: padding index file with %d '0' bytes", delta);
                 while (delta--)
                       writechar(f, 0);
                 }
              }
           else
              LOG_ERROR_STR(fileName);
           delete fileName;
           fileName = pFileExt = NULL;
           }
        }
     else
        esyslog(LOG_ERR, "ERROR: can't copy file name '%s'", FileName);
     }
}

cIndexFile::~cIndexFile()
{
  if (f >= 0)
     close(f);
  delete fileName;
}

bool cIndexFile::CatchUp(void)
{
  if (index && f >= 0) {
     struct stat buf;
     if (fstat(f, &buf) == 0) {
        int newLast = buf.st_size / sizeof(tIndex) - 1;
        if (newLast > last) {
           if (size <= newLast) {
              size *= 2;
              if (size <= newLast)
                 size = newLast + 1;
              }
           index = (tIndex *)realloc(index, size * sizeof(tIndex));
           if (index) {
              int offset = (last + 1) * sizeof(tIndex);
              int delta = (newLast - last) * sizeof(tIndex);
              if (lseek(f, offset, SEEK_SET) == offset) {
                 if (read(f, &index[last + 1], delta) != delta) {
                    esyslog(LOG_ERR, "ERROR: can't read from index");
                    delete index;
                    index = NULL;
                    close(f);
                    f = -1;
                    }
                 last = newLast;
                 return true;
                 }
              else
                 LOG_ERROR;
              }
           else
              esyslog(LOG_ERR, "ERROR: can't realloc() index");
           }
        }
     else
        LOG_ERROR;
     }
  return false;
}

void cIndexFile::Write(uchar PictureType, uchar FileNumber, int FileOffset)
{
  if (f >= 0) {
     tIndex i = { FileOffset, PictureType, FileNumber, 0 };
     if (write(f, &i, sizeof(i)) != sizeof(i)) {
        esyslog(LOG_ERR, "ERROR: can't write to index file");
        close(f);
        f = -1;
        return;
        }
     last++;
     }
}

bool cIndexFile::Get(int Index, uchar *FileNumber, int *FileOffset, uchar *PictureType, int *Length)
{
  if (index) {
     CatchUp();
     if (Index >= 0 && Index <= last) {
        *FileNumber = index[Index].number;
        *FileOffset = index[Index].offset;
        if (PictureType)
           *PictureType = index[Index].type;
        if (Length) {
           int fn = index[Index + 1].number;
           int fo = index[Index + 1].offset;
           if (fn == *FileNumber)
              *Length = fo - *FileOffset;
           else
              *Length = -1; // this means "everything up to EOF" (the buffer's Read function will act accordingly)
           }
        return true;
        }
     }
  return false;
}

int cIndexFile::GetNextIFrame(int Index, bool Forward, uchar *FileNumber, int *FileOffset, int *Length)
{
  if (index) {
     if (Forward)
        CatchUp();
     int d = Forward ? 1 : -1;
     for (;;) {
         Index += d;
         if (Index >= 0 && Index <= last - 100) { // '- 100': need to stay off the end!
            if (index[Index].type == I_FRAME) {
               if (FileNumber)
                  *FileNumber = index[Index].number;
               else
                  FileNumber = &index[Index].number;
               if (FileOffset)
                  *FileOffset = index[Index].offset;
               else
                  FileOffset = &index[Index].offset;
               if (Length) {
                  // all recordings end with a non-I_FRAME, so the following should be safe:
                  int fn = index[Index + 1].number;
                  int fo = index[Index + 1].offset;
                  if (fn == *FileNumber)
                     *Length = fo - *FileOffset;
                  else {
                     esyslog(LOG_ERR, "ERROR: 'I' frame at end of file #%d", *FileNumber);
                     *Length = -1;
                     }
                  }
               return Index;
               }
            }
         else
            break;
         }
     }
  return -1;
}

int cIndexFile::Get(uchar FileNumber, int FileOffset)
{
  if (index) {
     CatchUp();
     //TODO implement binary search!
     int i;
     for (i = 0; i < last; i++) {
         if (index[i].number > FileNumber || (index[i].number == FileNumber) && index[i].offset >= FileOffset)
            break;
         }
     return i;
     }
  return -1;
}

// --- cFileName -------------------------------------------------------------

class cFileName {
private:
  int file;
  int fileNumber;
  char *fileName, *pFileNumber;
  bool record;
  bool blocking;
public:
  cFileName(const char *FileName, bool Record, bool Blocking = false);
  ~cFileName();
  const char *Name(void) { return fileName; }
  int Number(void) { return fileNumber; }
  int Open(void);
  void Close(void);
  int SetOffset(int Number, int Offset = 0);
  int NextFile(void);
  };

cFileName::cFileName(const char *FileName, bool Record, bool Blocking)
{
  file = -1;
  fileNumber = 0;
  record = Record;
  blocking = Blocking;
  // Prepare the file name:
  fileName = new char[strlen(FileName) + RECORDFILESUFFIXLEN];
  if (!fileName) {
     esyslog(LOG_ERR, "ERROR: can't copy file name '%s'", fileName);
     return;
     }
  strcpy(fileName, FileName);
  pFileNumber = fileName + strlen(fileName);
  SetOffset(1);
}

cFileName::~cFileName()
{
  Close();
  delete fileName;
}

int cFileName::Open(void)
{
  if (file < 0) {
     int BlockingFlag = blocking ? 0 : O_NONBLOCK;
     if (record) {
        dsyslog(LOG_INFO, "recording to '%s'", fileName);
        file = OpenVideoFile(fileName, O_RDWR | O_CREAT | BlockingFlag);
        if (file < 0)
           LOG_ERROR_STR(fileName);
        }
     else {
        if (access(fileName, R_OK) == 0) {
           dsyslog(LOG_INFO, "playing '%s'", fileName);
           file = open(fileName, O_RDONLY | BlockingFlag);
           if (file < 0)
              LOG_ERROR_STR(fileName);
           }
        else if (errno != ENOENT)
           LOG_ERROR_STR(fileName);
        }
     }
  return file;
}

void cFileName::Close(void)
{
  if (file >= 0) {
     if ((record && CloseVideoFile(file) < 0) || (!record && close(file) < 0))
        LOG_ERROR_STR(fileName);
     file = -1;
     }
}

int cFileName::SetOffset(int Number, int Offset)
{
  if (fileNumber != Number)
     Close();
  if (0 < Number && Number <= MAXFILESPERRECORDING) {
     fileNumber = Number;
     sprintf(pFileNumber, RECORDFILESUFFIX, fileNumber);
     if (record) {
        if (access(fileName, F_OK) == 0) // file exists, let's try next suffix
           return SetOffset(Number + 1);
        else if (errno != ENOENT) { // something serious has happened
           LOG_ERROR_STR(fileName);
           return -1;
           }
        // found a non existing file suffix
        }
     if (Open() >= 0) {
        if (!record && Offset >= 0 && lseek(file, Offset, SEEK_SET) != Offset) {
           LOG_ERROR_STR(fileName);
           return -1;
           }
        }
     return file;
     }
  esyslog(LOG_ERR, "ERROR: max number of files (%d) exceeded", MAXFILESPERRECORDING);
  return -1;
}

int cFileName::NextFile(void)
{
  return SetOffset(fileNumber + 1);
}

// --- cRecordBuffer ---------------------------------------------------------

class cRecordBuffer : public cRingBufferLinear {
private:
  cDvbApi *dvbApi;
  cFileName fileName;
  cIndexFile *index;
  cRemux remux;
  uchar pictureType;
  int fileSize;
  int videoDev;
  int recordFile;
  bool recording;
  time_t lastDiskSpaceCheck;
  bool RunningLowOnDiskSpace(void);
  bool NextFile(void);
protected:
  virtual void Input(void);
  virtual void Output(void);
public:
  cRecordBuffer(cDvbApi *DvbApi, const char *FileName, int VPid, int APid1, int APid2, int DPid1, int DPid2);
  virtual ~cRecordBuffer();
  };

cRecordBuffer::cRecordBuffer(cDvbApi *DvbApi, const char *FileName, int VPid, int APid1, int APid2, int DPid1, int DPid2)
:cRingBufferLinear(VIDEOBUFSIZE, true)
,fileName(FileName, true)
,remux(VPid, APid1, APid2, DPid1, DPid2, true)
{
  dvbApi = DvbApi;
  index = NULL;
  pictureType = NO_PICTURE;
  fileSize = 0;
  recordFile = fileName.Open();
  recording = false;
  lastDiskSpaceCheck = time(NULL);
  if (!fileName.Name())
     return;
  // Create the index file:
  index = new cIndexFile(FileName, true);
  if (!index)
     esyslog(LOG_ERR, "ERROR: can't allocate index");
     // let's continue without index, so we'll at least have the recording
  videoDev = dvbApi->SetModeRecord();
  Start();
}

cRecordBuffer::~cRecordBuffer()
{
  Stop();
  dvbApi->SetModeNormal(true);
  delete index;
}

bool cRecordBuffer::RunningLowOnDiskSpace(void)
{
  if (time(NULL) > lastDiskSpaceCheck + DISKCHECKINTERVAL) {
     uint Free = FreeDiskSpaceMB(fileName.Name());
     lastDiskSpaceCheck = time(NULL);
     if (Free < MINFREEDISKSPACE) {
        dsyslog(LOG_INFO, "low disk space (%d MB, limit is %d MB)", Free, MINFREEDISKSPACE);
        return true;
        }
     }
  return false;
}

bool cRecordBuffer::NextFile(void)
{
  if (recordFile >= 0 && pictureType == I_FRAME) { // every file shall start with an I_FRAME
     if (fileSize > MAXVIDEOFILESIZE || RunningLowOnDiskSpace()) {
        recordFile = fileName.NextFile();
        fileSize = 0;
        }
     }
  return recordFile >= 0;
}

void cRecordBuffer::Input(void)
{
  dsyslog(LOG_INFO, "input thread started (pid=%d)", getpid());

  uchar b[MINVIDEODATA];
  time_t t = time(NULL);
  recording = true;
  for (;;) {
      int r = read(videoDev, b, sizeof(b));
      if (r > 0) {
         uchar *p = b;
         while (r > 0) {
               int w = Put(p, r);
               p += w;
               r -= w;
               }
         t = time(NULL);
         }
      else if (r < 0) {
         if (errno != EAGAIN) {
            LOG_ERROR;
            if (errno != EBUFFEROVERFLOW)
               break;
            }
         }
      if (time(NULL) - t > MAXBROKENTIMEOUT) {
         esyslog(LOG_ERR, "ERROR: video data stream broken");
         cThread::EmergencyExit(true);
         t = time(NULL);
         }
      cFile::FileReady(videoDev, 100);
      if (!recording)
         break;
      }

  dsyslog(LOG_INFO, "input thread ended (pid=%d)", getpid());
}

void cRecordBuffer::Output(void)
{
  dsyslog(LOG_INFO, "output thread started (pid=%d)", getpid());

  uchar b[MINVIDEODATA];
  int r = 0;
  for (;;) {
      int g = Get(b + r, sizeof(b) - r);
      if (g > 0) {
         r += g;
         int Count = r, Result;
         const uchar *p = remux.Process(b, Count, Result, &pictureType);
         if (p) {
            if (!Busy() && pictureType == I_FRAME) // finish the recording before the next 'I' frame
               break;
            if (NextFile()) {
               if (index && pictureType != NO_PICTURE)
                  index->Write(pictureType, fileName.Number(), fileSize);
               while (Result > 0) {
                     int w = write(recordFile, p, Result);
                     if (w < 0) {
                        LOG_ERROR_STR(fileName.Name());
                        recording = false;
                        return;
                        }
                     p += w;
                     Result -= w;
                     fileSize += w;
                     }
               }
            else
               break;
            }
         if (Count > 0) {
            r -= Count;
            memmove(b, b + Count, r);
            }
         if (!recording)
            break;
         }
      else
         usleep(1); // this keeps the CPU load low
      }
  recording = false;

  dsyslog(LOG_INFO, "output thread ended (pid=%d)", getpid());
}

// --- ReadFrame -------------------------------------------------------------

int ReadFrame(int f, uchar *b, int Length, int Max)
{
  if (Length == -1)
     Length = Max; // this means we read up to EOF (see cIndex)
  else if (Length > Max) {
     esyslog(LOG_ERR, "ERROR: frame larger than buffer (%d > %d)", Length, Max);
     Length = Max;
     }
  int r = read(f, b, Length);
  if (r < 0)
     LOG_ERROR;
  return r;
}

// --- cPlayBuffer ---------------------------------------------------------

class cPlayBuffer : public cRingBufferFrame {
protected:
  cDvbApi *dvbApi;
  int videoDev, audioDev;
  FILE *dolbyDev;
  int blockInput, blockOutput;
  bool still, paused, fastForward, fastRewind;
  int readIndex, writeIndex;
  bool canDoTrickMode;
  bool canToggleAudioTrack;
  uchar audioTrack;
  virtual void Empty(bool Block = false);
  virtual void StripAudioPackets(uchar *b, int Length, uchar Except = 0x00) {}
  virtual void Output(void);
public:
  cPlayBuffer(cDvbApi *DvbApi, int VideoDev, int AudioDev);
  virtual ~cPlayBuffer();
  virtual void Pause(void);
  virtual void Play(void);
  virtual void Forward(void);
  virtual void Backward(void);
  virtual int SkipFrames(int Frames) { return -1; }
  virtual void SkipSeconds(int Seconds) {}
  virtual void Goto(int Position, bool Still = false) {}
  virtual void GetIndex(int &Current, int &Total, bool SnapToIFrame = false) { Current = Total = -1; }
  bool CanToggleAudioTrack(void) { return canToggleAudioTrack; };
  virtual void ToggleAudioTrack(void);
  };

cPlayBuffer::cPlayBuffer(cDvbApi *DvbApi, int VideoDev, int AudioDev)
:cRingBufferFrame(VIDEOBUFSIZE)
{
  dvbApi = DvbApi;
  videoDev = VideoDev;
  audioDev = AudioDev;
  dolbyDev = NULL;
  blockInput = blockOutput = false;
  still = paused = fastForward = fastRewind = false;
  readIndex = writeIndex = -1;
  canDoTrickMode = false;
  canToggleAudioTrack = false;
  audioTrack = 0xC0;
  if (cDvbApi::AudioCommand()) {
     dolbyDev = popen(cDvbApi::AudioCommand(), "w");
     if (!dolbyDev)
        esyslog(LOG_ERR, "ERROR: can't open pipe to audio command '%s'", cDvbApi::AudioCommand());
     }
}

cPlayBuffer::~cPlayBuffer()
{
  if (dolbyDev)
     pclose(dolbyDev);
}

void cPlayBuffer::Output(void)
{
  dsyslog(LOG_INFO, "output thread started (pid=%d)", getpid());

  while (Busy()) {
        if (blockOutput) {
           if (blockOutput > 1)
              blockOutput = 1;
           continue;
           }
        const cFrame *frame = Get();
        if (frame) {
           StripAudioPackets((uchar *)frame->Data(), frame->Count(), (fastForward || fastRewind) ? 0x00 : audioTrack);//XXX
           for (int i = 0; i < ((paused && fastRewind) ? 24 : 1); i++) { // show every I_FRAME 24 times in slow rewind mode to achieve roughly the same speed as in slow forward mode
               const uchar *p = frame->Data();
               int r = frame->Count();
               while (r > 0 && Busy() && !blockOutput) {
                     cFile::FileReadyForWriting(videoDev, 100);
                     int w = write(videoDev, p, r);
                     if (w > 0) {
                        p += w;
                        r -= w;
                        }
                     else if (w < 0 && errno != EAGAIN) {
                        LOG_ERROR;
                        Stop();
                        return;
                        }
                     }
               writeIndex = frame->Index();
               }
           Drop(frame);
           }
        }

  dsyslog(LOG_INFO, "output thread ended (pid=%d)", getpid());
}

void cPlayBuffer::Empty(bool Block)
{
  if (!(blockInput || blockOutput)) {
     blockInput = blockOutput = 2;
     EnablePut();
     EnableGet();
     time_t t0 = time(NULL);
     while ((blockInput > 1 || blockOutput > 1) && time(NULL) - t0 < 2)
           usleep(1);
     Lock();
     readIndex = writeIndex;
     cRingBufferFrame::Clear();
     CHECK(ioctl(videoDev, VIDEO_CLEAR_BUFFER));
     CHECK(ioctl(audioDev, AUDIO_CLEAR_BUFFER));
     }
  if (!Block) {
     blockInput = blockOutput = 0;
     Unlock();
     }
}

void cPlayBuffer::Pause(void)
{
  paused = !paused;
  bool empty = fastForward || fastRewind;
  if (empty)
     Empty(true);
  fastForward = fastRewind = false;
  CHECK(ioctl(videoDev, paused ? VIDEO_FREEZE : VIDEO_CONTINUE));
  CHECK(ioctl(audioDev, AUDIO_SET_MUTE, paused));
  still = false;
  if (empty)
     Empty(false);
}

void cPlayBuffer::Play(void)
{
  if (fastForward || fastRewind || paused) {
     bool empty = !paused || fastRewind;
     if (empty)
        Empty(true);
     still = false;
     CHECK(ioctl(videoDev, paused ? VIDEO_CONTINUE : VIDEO_PLAY));
     CHECK(ioctl(audioDev, AUDIO_SET_AV_SYNC, true));
     CHECK(ioctl(audioDev, AUDIO_SET_MUTE, false));
     if (empty)
        Empty(false);
     fastForward = fastRewind = paused = false;
     }
}

void cPlayBuffer::Forward(void)
{
  if (canDoTrickMode || paused) {
     bool empty = !paused || fastRewind;
     if (empty) {
        Empty(true);
        if (fastForward)
           readIndex -= 150; // this about compensates for the buffered data, so that we don't get too far ahead
        }
     still = false;
     fastForward = !fastForward;
     fastRewind = false;
     if (paused)
        CHECK(ioctl(videoDev, fastForward ? VIDEO_SLOWMOTION : VIDEO_FREEZE, 2));
     CHECK(ioctl(audioDev, AUDIO_SET_AV_SYNC, !fastForward));
     CHECK(ioctl(audioDev, AUDIO_SET_MUTE, fastForward || paused));
     if (empty)
        Empty(false);
     }
}

void cPlayBuffer::Backward(void)
{
  if (canDoTrickMode) {
     Empty(true);
     still = false;
     fastRewind = !fastRewind;
     fastForward = false;
     if (paused)
        CHECK(ioctl(videoDev, fastRewind ? VIDEO_CONTINUE : VIDEO_FREEZE));
     CHECK(ioctl(audioDev, AUDIO_SET_AV_SYNC, !fastRewind));
     CHECK(ioctl(audioDev, AUDIO_SET_MUTE, fastRewind || paused));
     Empty(false);
     }
}

void cPlayBuffer::ToggleAudioTrack(void)
{
  if (CanToggleAudioTrack()) {
     audioTrack = (audioTrack == 0xC0) ? 0xC1 : 0xC0;
     Empty();
     }
}

// --- cReplayBuffer ---------------------------------------------------------

class cReplayBuffer : public cPlayBuffer {
private:
  cIndexFile *index;
  cFileName fileName;
  int replayFile;
  bool eof;
  bool NextFile(uchar FileNumber = 0, int FileOffset = -1);
  void Close(void);
  virtual void StripAudioPackets(uchar *b, int Length, uchar Except = 0x00);
  void DisplayFrame(uchar *b, int Length);
  int Resume(void);
  bool Save(void);
protected:
  virtual void Input(void);
public:
  cReplayBuffer(cDvbApi *DvbApi, int VideoDev, int AudioDev, const char *FileName);
  virtual ~cReplayBuffer();
  virtual int SkipFrames(int Frames);
  virtual void SkipSeconds(int Seconds);
  virtual void Goto(int Position, bool Still = false);
  virtual void GetIndex(int &Current, int &Total, bool SnapToIFrame = false);
  };

cReplayBuffer::cReplayBuffer(cDvbApi *DvbApi, int VideoDev, int AudioDev, const char *FileName)
:cPlayBuffer(DvbApi, VideoDev, AudioDev)
,fileName(FileName, false)
{
  index = NULL;
  replayFile = fileName.Open();
  eof = false;
  if (!fileName.Name())
     return;
  // Create the index file:
  index = new cIndexFile(FileName, false);
  if (!index) {
     esyslog(LOG_ERR, "ERROR: can't allocate index");
     }
  else if (!index->Ok()) {
     delete index;
     index = NULL;
     }
  canDoTrickMode = index != NULL;
  dvbApi->SetModeReplay();
  Start();
}

cReplayBuffer::~cReplayBuffer()
{
  Stop();
  Save();
  Close();
  dvbApi->SetModeNormal(false);
  delete index;
}

void cReplayBuffer::Input(void)
{
  dsyslog(LOG_INFO, "input thread started (pid=%d)", getpid());

  readIndex = Resume();
  if (readIndex >= 0)
     isyslog(LOG_INFO, "resuming replay at index %d (%s)", readIndex, IndexToHMSF(readIndex, true));

  uchar b[MAXFRAMESIZE];
  while (Busy() && (blockInput || NextFile())) {
        if (blockInput) {
           if (blockInput > 1)
              blockInput = 1;
           continue;
           }
        if (!still) {
           int r = 0;
           if (fastForward && !paused || fastRewind) {
              uchar FileNumber;
              int FileOffset, Length;
              int Index = index->GetNextIFrame(readIndex, fastForward, &FileNumber, &FileOffset, &Length);
              if (Index >= 0) {
                 if (!NextFile(FileNumber, FileOffset))
                    break;
                 }
              else {
                 paused = fastForward = fastRewind = false;
                 Play();
                 continue;
                 }
              readIndex = Index;
              r = ReadFrame(replayFile, b, Length, sizeof(b));
              }
           else if (index) {
              uchar FileNumber;
              int FileOffset, Length;
              readIndex++;
              if (!(index->Get(readIndex, &FileNumber, &FileOffset, NULL, &Length) && NextFile(FileNumber, FileOffset)))
                 break;
              r = ReadFrame(replayFile, b, Length, sizeof(b));
              }
           else // allows replay even if the index file is missing
              r = read(replayFile, b, sizeof(b));
           if (r > 0) {
              cFrame *frame = new cFrame(b, r, readIndex);
              while (Busy() && !blockInput && !Put(frame))
                    ;
              }
           else if (r == 0)
              eof = true;
           else if (r < 0 && errno != EAGAIN) {
              LOG_ERROR;
              break;
              }
           }
        else//XXX
           usleep(1); // this keeps the CPU load low
        }

  dsyslog(LOG_INFO, "input thread ended (pid=%d)", getpid());
}

void cReplayBuffer::StripAudioPackets(uchar *b, int Length, uchar Except)
{
  if (canDoTrickMode) {
     for (int i = 0; i < Length - 6; i++) {
         if (b[i] == 0x00 && b[i + 1] == 0x00 && b[i + 2] == 0x01) {
            uchar c = b[i + 3];
            int l = b[i + 4] * 256 + b[i + 5] + 6;
            switch (c) {
              case 0xBD: // dolby
                   if (Except && dolbyDev) {
                      int written = b[i + 8] + 9; // skips the PES header
                      int n = l - written;
                      while (n > 0) {
                            int w = fwrite(&b[i + written], 1, n, dolbyDev);
                            if (w < 0) {
                               LOG_ERROR;
                               break;
                               }
                            n -= w;
                            written += w;
                            }
                      }
                   // continue with deleting the data - otherwise it disturbs DVB replay
              case 0xC0 ... 0xC1: // audio
                   if (c == 0xC1)
                      canToggleAudioTrack = true;
                   if (!Except || c != Except) {
                      int n = l;
                      for (int j = i; j < Length && n--; j++)
                          b[j] = 0x00;
                      }
                   break;
              case 0xE0 ... 0xEF: // video
                   break;
              default:
                   //esyslog(LOG_ERR, "ERROR: unexpected packet id %02X", c);
                   l = 0;
              }
            if (l)
               i += l - 1; // the loop increments, too!
            }
         /*XXX
         else
            esyslog(LOG_ERR, "ERROR: broken packet header");
            XXX*/
         }
     }
}

void cReplayBuffer::DisplayFrame(uchar *b, int Length)
{
  StripAudioPackets(b, Length);
  videoDisplayStillPicture sp = { (char *)b, Length };
  CHECK(ioctl(audioDev, AUDIO_SET_AV_SYNC, false));
  CHECK(ioctl(audioDev, AUDIO_SET_MUTE, true));
  CHECK(ioctl(videoDev, VIDEO_STILLPICTURE, &sp));
}

void cReplayBuffer::Close(void)
{
  if (replayFile >= 0) {
     fileName.Close();
     replayFile = -1;
     }
}

int cReplayBuffer::Resume(void)
{
  if (index) {
     int Index = index->GetResume();
     if (Index >= 0) {
        uchar FileNumber;
        int FileOffset;
        if (index->Get(Index, &FileNumber, &FileOffset) && NextFile(FileNumber, FileOffset))
           return Index;
        }
     }
  return -1;
}

bool cReplayBuffer::Save(void)
{
  if (index) {
     int Index = writeIndex;
     if (Index >= 0) {
        Index -= RESUMEBACKUP;
        if (Index > 0)
           Index = index->GetNextIFrame(Index, false);
        else
           Index = 0;
        if (Index >= 0)
           return index->StoreResume(Index);
        }
     }
  return false;
}

int cReplayBuffer::SkipFrames(int Frames)
{
  if (index && Frames) {
     int Current, Total;
     GetIndex(Current, Total, true);
     int OldCurrent = Current;
     Current = index->GetNextIFrame(Current + Frames, Frames > 0);
     return Current >= 0 ? Current : OldCurrent;
     }
  return -1;
}

void cReplayBuffer::SkipSeconds(int Seconds)
{
  if (index && Seconds) {
     Empty(true);
     int Index = writeIndex;
     if (Index >= 0) {
        if (Seconds < 0) {
           int sec = index->Last() / FRAMESPERSEC;
           if (Seconds < -sec)
              Seconds = -sec;
           }
        Index += Seconds * FRAMESPERSEC;
        if (Index < 0)
           Index = 1; // not '0', to allow GetNextIFrame() below to work!
        uchar FileNumber;
        int FileOffset;
        readIndex = writeIndex = index->GetNextIFrame(Index, false, &FileNumber, &FileOffset) - 1; // Input() will first increment it!
        }
     Empty(false);
     Play();
     }
}

void cReplayBuffer::Goto(int Index, bool Still)
{
  if (index) {
     Empty(true);
     if (paused)
        CHECK(ioctl(videoDev, VIDEO_CONTINUE));
     if (++Index <= 0)
        Index = 1; // not '0', to allow GetNextIFrame() below to work!
     uchar FileNumber;
     int FileOffset, Length;
     Index = index->GetNextIFrame(Index, false, &FileNumber, &FileOffset, &Length);
     if (Index >= 0 && NextFile(FileNumber, FileOffset) && Still) {
        still = true;
        uchar b[MAXFRAMESIZE];
        int r = ReadFrame(replayFile, b, Length, sizeof(b));
        if (r > 0)
           DisplayFrame(b, r);
        paused = true;
        }
     else
        still = false;
     readIndex = writeIndex = Index;
     Empty(false);
     }
}

void cReplayBuffer::GetIndex(int &Current, int &Total, bool SnapToIFrame)
{
  if (index) {
     if (still)
        Current = readIndex;
     else {
        Current = writeIndex;
        if (SnapToIFrame) {
           int i1 = index->GetNextIFrame(Current + 1, false);
           int i2 = index->GetNextIFrame(Current, true);
           Current = (abs(Current - i1) <= abs(Current - i2)) ? i1 : i2;
           }
        }
     Total = index->Last();
     }
  else
     Current = Total = -1;
}

bool cReplayBuffer::NextFile(uchar FileNumber, int FileOffset)
{
  if (FileNumber > 0)
     replayFile = fileName.SetOffset(FileNumber, FileOffset);
  else if (replayFile >= 0 && eof) {
     Close();
     replayFile = fileName.NextFile();
     }
  eof = false;
  return replayFile >= 0;
}

#ifdef DVDSUPPORT
// --- cDVDplayBuffer --------------------------------------------------------

class cDVDplayBuffer : public cPlayBuffer {
private:
  uchar audioTrack;

  cDVD *dvd;//XXX necessary???

  int titleid;
  int chapid;
  int angle;
  dvd_file_t *title;
  ifo_handle_t *vmg_file;
  ifo_handle_t *vts_file;

  int doplay;
  int cyclestate;
  int prevcycle;
  int brakeCounter;
  int skipCnt;

  tt_srpt_t *tt_srpt;
  vts_ptt_srpt_t *vts_ptt_srpt;
  pgc_t *cur_pgc;
  dsi_t dsi_pack;
  unsigned int next_vobu;
  unsigned int prev_vobu;
  unsigned int next_ilvu_start;
  unsigned int cur_output_size;
  unsigned int min_output_size;
  unsigned int pktcnt;
  int pgc_id;
  int start_cell;
  int next_cell;
  int prev_cell;
  int cur_cell;
  unsigned int cur_pack;
  int ttn;
  int pgn;

  uchar *data;

  int logAudioTrack;
  int maxAudioTrack;

  ac3_config_t ac3_config;
  enum { AC3_STOP, AC3_START, AC3_PLAY } ac3stat;
  uchar *ac3data;
  int ac3inp;
  int ac3outp;
  int lpcm_count;
  int is_nav_pack(unsigned char *buffer);
  void Close(void);
  virtual void Empty(bool Block = false);
  int decode_packet(unsigned char *sector, int iframe);
  int ScanVideoPacket(const uchar *Data, int Count, uchar *PictureType);
  bool PacketStart(uchar **Data, int len);
  int GetPacketType(const uchar *Data);
  int GetStuffingLen(const uchar *Data);
  int GetPacketLength(const uchar *Data);
  int GetPESHeaderLength(const uchar *Data);
  int SendPCM(int size);
  void playDecodedAC3(void);
  void handleAC3(unsigned char *sector, int length);
  void putFrame(unsigned char *sector, int length);
  unsigned int getAudioStream(unsigned int StreamId);
  void setChapid(void);
  void NextState(int State) { prevcycle = cyclestate; cyclestate = State; }
protected:
  virtual void Input(void);
public:
  cDVDplayBuffer(cDvbApi *DvbApi, int VideoDev, int AudioDev, cDVD *DvD, int title);
  virtual ~cDVDplayBuffer();
  virtual int SkipFrames(int Frames);
  virtual void SkipSeconds(int Seconds);
  virtual void Goto(int Position, bool Still = false);
  virtual void GetIndex(int &Current, int &Total, bool SnapToIFrame = false);
  virtual void ToggleAudioTrack(void);
  };

#define cOPENDVD         0
#define cOPENTITLE       1
#define cOPENCHAPTER     2
#define cOUTCELL         3
#define cREADFRAME       4
#define cOUTPACK         5
#define cOUTFRAMES       6

#define aAC3          0x80
#define aLPCM         0xA0

cDVDplayBuffer::cDVDplayBuffer(cDvbApi *DvbApi, int VideoDev, int AudioDev, cDVD *DvD, int title)
:cPlayBuffer(DvbApi, VideoDev, AudioDev)
{
  dvd = DvD;
  titleid = title;
  chapid = 0;
  angle = 0;
  cyclestate = cOPENDVD;
  prevcycle = 0;
  brakeCounter = 0;
  skipCnt = 0;
  logAudioTrack = 0;
  canToggleAudioTrack = true;//XXX determine from cDVD!
  ac3_config.num_output_ch = 2;
  //    ac3_config.flags = /* mm_accel() | */ MM_ACCEL_MLIB;
  ac3_config.flags = 0;
  ac3_init(&ac3_config);
  data = new uchar[1024 * DVD_VIDEO_LB_LEN];
  ac3data = new uchar[AC3_BUFFER_SIZE];
  ac3inp = ac3outp = 0;
  ac3stat = AC3_START;
  canDoTrickMode = true;
  dvbApi->SetModeReplay();
  Start();
}

cDVDplayBuffer::~cDVDplayBuffer()
{
  Stop();
  Close();
  dvbApi->SetModeNormal(false);
  delete ac3data;
  delete data;
}

unsigned int cDVDplayBuffer::getAudioStream(unsigned int StreamId)
{
  unsigned int trackID;

  if ((cyclestate < cOPENCHAPTER) || (StreamId > 7))
     return 0;
  if (!(cur_pgc->audio_control[StreamId] & 0x8000))
     return 0;
  int track = (cur_pgc->audio_control[StreamId] >> 8) & 0x07;
  switch (vts_file->vtsi_mat->vts_audio_attr[track].audio_format) {
    case 0: // ac3
            trackID = aAC3;
            break;
    case 2: // mpeg1
    case 3: // mpeg2ext
    case 4: // lpcm
    case 6: // dts
            trackID = aLPCM;
            break;
    default: esyslog(LOG_ERR, "ERROR: unknown Audio stream info");
             return 0;
    }
  trackID |= track;
  return trackID;
}

void cDVDplayBuffer::ToggleAudioTrack(void)
{
  unsigned int newTrack;

  if (CanToggleAudioTrack() && maxAudioTrack != 0) {
     logAudioTrack = (logAudioTrack + 1) % maxAudioTrack;
     if ((newTrack = getAudioStream(logAudioTrack)) != 0)
        audioTrack = newTrack;
#ifdef DVDDEBUG
     dsyslog(LOG_INFO, "DVB: Audio Stream ID changed to: %x", audioTrack);
#endif
     ac3stat = AC3_START;
     ac3outp = ac3inp;
     }
}

/**
 * Returns true if the pack is a NAV pack.  This check is clearly insufficient,
 * and sometimes we incorrectly think that valid other packs are NAV packs.  I
 * need to make this stronger.
 */
inline int cDVDplayBuffer::is_nav_pack(unsigned char *buffer)
{
  return buffer[41] == 0xbf && buffer[1027] == 0xbf;
}

void cDVDplayBuffer::Input(void)
{
  dsyslog(LOG_INFO, "input thread started (pid=%d)", getpid());

  doplay = true;
  while (Busy() && doplay) {
        if (blockInput) {
           if (blockInput > 1)
              blockInput = 1;
           continue;
           }

        //BEGIN: ripped from play_title

        /**
         * Playback by cell in this pgc, starting at the cell for our chapter.
         */

        //dsyslog(LOG_INFO, "DVD: cyclestate: %d", cyclestate);
        switch (cyclestate) {

          case cOPENDVD: // open the DVD and get all the basic information
               {
                 if (!dvd->isValid()) {
                    doplay = false;
                    break;
                    }

                 /**
                  * Load the video manager to find out the information about the titles on
                  * this disc.
                  */
                 vmg_file = dvd->openVMG();
                 if (!vmg_file) {
                    esyslog(LOG_ERR, "ERROR: can't open VMG info");
                    doplay = false;
                    break;
                    }
                 tt_srpt = vmg_file->tt_srpt;

                 NextState(cOPENTITLE);
                 break;
               }

          case cOPENTITLE: // open the selected title
               {
                 /**
                  * Make sure our title number is valid.
                  */
                 isyslog(LOG_INFO, "DVD: there are %d titles on this DVD", tt_srpt->nr_of_srpts);
                 if (titleid < 0 || titleid >= tt_srpt->nr_of_srpts) {
                    esyslog(LOG_ERR, "ERROR: invalid title %d", titleid + 1);
                    doplay = false;
                    break;
                    }

                 /**
                  * Load the VTS information for the title set our title is in.
                  */
                 vts_file = dvd->openVTS(tt_srpt->title[titleid].title_set_nr);
                 if (!vts_file) {
                    esyslog(LOG_ERR, "ERROR: can't open the title %d info file", tt_srpt->title[titleid].title_set_nr);
                    doplay = false;
                    break;
                    }

                 NextState(cOPENCHAPTER);
                 break;
               }

          case cOPENCHAPTER:
               {
                 /**
                  * Make sure the chapter number is valid for this title.
                  */
                 isyslog(LOG_INFO, "DVD: there are %d chapters in this title", tt_srpt->title[titleid].nr_of_ptts);
                 if (chapid < 0 || chapid >= tt_srpt->title[titleid].nr_of_ptts) {
                    esyslog(LOG_ERR, "ERROR: invalid chapter %d", chapid + 1);
                    doplay = false;
                    break;
                    }

                 /**
                  * Determine which program chain we want to watch.  This is based on the
                  * chapter number.
                  */
                 ttn = tt_srpt->title[titleid].vts_ttn;
                 vts_ptt_srpt = vts_file->vts_ptt_srpt;
                 pgc_id = vts_ptt_srpt->title[ttn - 1].ptt[chapid].pgcn;
                 pgn = vts_ptt_srpt->title[ttn - 1].ptt[chapid].pgn;
                 cur_pgc = vts_file->vts_pgcit->pgci_srp[pgc_id - 1].pgc;
                 start_cell = cur_pgc->program_map[pgn - 1] - 1;

                 /**
                  * setup Audio information
                  **/
                 for (maxAudioTrack = 0; maxAudioTrack < 8; maxAudioTrack++) {
                     if (!(cur_pgc->audio_control[maxAudioTrack] & 0x8000))
                        break;
                     }
                 canToggleAudioTrack = (maxAudioTrack > 0);
                 // init the AudioInformation
                 audioTrack = getAudioStream(logAudioTrack);
#ifdef DVDDEBUG
                 dsyslog(LOG_INFO, "DVD: max: %d, track: %x", maxAudioTrack, audioTrack);
#endif

                 /**
                  * We've got enough info, time to open the title set data.
                  */
                 title = dvd->openTitle(tt_srpt->title[titleid].title_set_nr, DVD_READ_TITLE_VOBS);
                 if (!title) {
                    esyslog(LOG_ERR, "ERROR: can't open title VOBS (VTS_%02d_1.VOB).", tt_srpt->title[titleid].title_set_nr);
                    doplay = false;
                    break;
                    }

                 /**
                  * Playback by cell in this pgc, starting at the cell for our chapter.
                  */
                 next_cell = start_cell;
                 prev_cell = start_cell;
                 cur_cell  = start_cell;

                 NextState(cOUTCELL);
                 break;
               }

          case cOUTCELL:
               {
#ifdef DVDDEBUG
                 dsyslog(LOG_INFO, "DVD: new cell: %d", cur_cell);
                 dsyslog(LOG_INFO, "DVD: vob_id: %x, cell_nr: %x", cur_pgc->cell_position[cur_cell].vob_id_nr, cur_pgc->cell_position[cur_cell].cell_nr);
#endif

                 if (cur_cell < 0) {
                    cur_cell = 0;
                    Backward();
                    }
                 doplay = (cur_cell < cur_pgc->nr_of_cells);
                 if (!doplay)
                    break;

                 /* Check if we're entering an angle block. */
                 if (cur_pgc->cell_playback[cur_cell].block_type == BLOCK_TYPE_ANGLE_BLOCK) {
                    cur_cell += angle;
                    for (int i = 0; ; ++i) {
                        if (cur_pgc->cell_playback[cur_cell + i].block_mode == BLOCK_MODE_LAST_CELL) {
                           next_cell = cur_cell + i + 1;
                           break;
                           }
                        }
                    }
                 else {
                    next_cell = cur_cell + 1;
                    prev_cell = cur_cell - 1;
                    }

                 // init settings for next state
                 if (!fastRewind)
                    cur_pack = cur_pgc->cell_playback[cur_cell].first_sector;
                 else
                    cur_pack = cur_pgc->cell_playback[cur_cell].last_vobu_start_sector;

                 NextState(cOUTPACK);
                 break;
               }

          case cOUTPACK:
               {
#ifdef DVDDEBUG
                 dsyslog(LOG_INFO, "DVD: new pack: %d", cur_pack);
#endif
                 /**
                  * We loop until we're out of this cell.
                  */

                 if (!fastRewind) {
                    if (cur_pack >= cur_pgc->cell_playback[cur_cell].last_sector) {
                       cur_cell = next_cell;
#ifdef DVDDEBUG
                       dsyslog(LOG_INFO, "DVD: end of pack");
#endif
                       NextState(cOUTCELL);
                       break;
                       }
                    }
                 else {
#ifdef DVDDEBUG
                    dsyslog(LOG_INFO, "DVD: prev: %d, curr: %x, next: %x, prev: %x", prevcycle, cur_pack, next_vobu, prev_vobu);
#endif
                    if ((cur_pack & 0x80000000) != 0) {
                       cur_cell = prev_cell;
#ifdef DVDDEBUG
                       dsyslog(LOG_INFO, "DVD: start of pack");
#endif
                       NextState(cOUTCELL);
                       break;
                       }
                    }

                 /**
                  * Read NAV packet.
                  */
                 int len = DVDReadBlocks(title, cur_pack, 1, data);
                 if (len == 0) {
                    esyslog(LOG_ERR, "ERROR: read failed for block %d", cur_pack);
                    doplay = false;
                    break;
                    }
                 if (!is_nav_pack(data)) {
                    esyslog(LOG_ERR, "ERROR: no nav_pack");
                    return;
                    }

                 /**
                  * Parse the contained dsi packet.
                  */
                 navRead_DSI(&dsi_pack, &(data[DSI_START_BYTE]), sizeof(dsi_t));
                 if (cur_pack != dsi_pack.dsi_gi.nv_pck_lbn) {
                    esyslog(LOG_ERR, "ERROR: cur_pack != dsi_pack.dsi_gi.nv_pck_lbn");
                    return;
                    }
                 // navPrint_DSI(&dsi_pack);

                 /**
                  * Determine where we go next.  These values are the ones we mostly
                  * care about.
                  */
                 next_ilvu_start = cur_pack + dsi_pack.sml_agli.data[angle].address;
                 cur_output_size = dsi_pack.dsi_gi.vobu_ea;
                 min_output_size = dsi_pack.dsi_gi.vobu_1stref_ea;

                 /**
                  * If we're not at the end of this cell, we can determine the next
                  * VOBU to display using the VOBU_SRI information section of the
                  * DSI.  Using this value correctly follows the current angle,
                  * avoiding the doubled scenes in The Matrix, and makes our life
                  * really happy.
                  *
                  * Otherwise, we set our next address past the end of this cell to
                  * force the code above to go to the next cell in the program.
                  */
                 if (dsi_pack.vobu_sri.next_vobu != SRI_END_OF_CELL)
                    next_vobu = cur_pack + (dsi_pack.vobu_sri.next_vobu & 0x7fffffff);
                 else
                    next_vobu = cur_pack + cur_output_size + 1;

                 if (dsi_pack.vobu_sri.prev_vobu != SRI_END_OF_CELL)
                    prev_vobu = cur_pack - (dsi_pack.vobu_sri.prev_vobu & 0x7fffffff);
                 else {
#ifdef DVDDEBUG
                    dsyslog(LOG_INFO, "DVD: cur: %x, prev: %x", cur_pack, dsi_pack.vobu_sri.prev_vobu);
#endif
                    prev_vobu =  0x80000000;
                    }

#ifdef DVDDEBUG
                 dsyslog(LOG_INFO, "DVD: curr: %x, next: %x, prev: %x", cur_pack, next_vobu, prev_vobu);
#endif
                 if (cur_output_size >= 1024) {
                    esyslog(LOG_ERR, "ERROR: cur_output_size >= 1024");
                    return;
                    }
                 cur_pack++;

                 NextState(cREADFRAME);
                 break;
               }

          case cREADFRAME:
               {
                 int trickMode = (fastForward && !paused || fastRewind);

                 /* FIXME:
                  *   the entire trickMode code relies on the assumtion
                  *   that there is only one I-FRAME per PACK
                  *
                  *   I have no clue wether that is correct or not !!!
                  */
                 if (trickMode && (skipCnt++ % 4 != 0)) {
                    cur_pack = (!fastRewind) ? next_vobu : prev_vobu;
                    NextState(cOUTPACK);
                    break;
                    }

                 if (trickMode)
                    cur_output_size = min_output_size;

                 /**
                  * Read in cursize packs.
                  */
#ifdef DVDDEBUG
                 dsyslog(LOG_INFO, "DVD: read pack: %d", cur_pack);
#endif
                 int len = DVDReadBlocks(title, cur_pack, cur_output_size, data);
                 if (len != (int)cur_output_size * DVD_VIDEO_LB_LEN) {
                    esyslog(LOG_ERR, "ERROR: read failed for %d blocks at %d", cur_output_size, cur_pack);
                    doplay = false;
                    break;
                    }
                 pktcnt = 0;
                 NextState(cOUTFRAMES);
                 break;
               }

          case cOUTFRAMES:
               {
                 int trickMode = (fastForward && !paused || fastRewind);

                 /**
                  * Output cursize packs.
                  */
                 if (pktcnt >= cur_output_size) {
                    cur_pack = next_vobu;
                    NextState(cOUTPACK);
                    break;
                    }
                 //dsyslog(LOG_INFO, "DVD: pack: %d, frame: %d", cur_pack, pktcnt);

                 if (decode_packet(&data[pktcnt * DVD_VIDEO_LB_LEN], trickMode) != 1) {   //we've got a video packet
                    if (trickMode) {
                        //dsyslog(LOG_INFO, "DVD: did pack: %d", pktcnt);
                        cur_pack = (!fastRewind) ? next_vobu : prev_vobu;
                        NextState(cOUTPACK);
                        break;
                        }
                    }

                 pktcnt++;

                 if (pktcnt >= cur_output_size) {
                    cur_pack = next_vobu;
                    NextState(cOUTPACK);
                    break;
                    }
                 break;
               }

        default:
               {
                 esyslog(LOG_ERR, "ERROR: cyclestate %d not known", cyclestate);
                 return;
               }
        }

        // dsyslog(LOG_INF, "DVD: new cyclestate: %d, pktcnt: %d, cur: %d", cyclestate, pktcnt, cur_output_size);
        }

  dsyslog(LOG_INFO, "input thread ended (pid=%d)", getpid());
}

#define NO_PICTURE 0
#define SC_PICTURE 0x00

inline bool cDVDplayBuffer::PacketStart(uchar **Data, int len)
{
  while (len > 6 && !((*Data)[0] == 0x00 && (*Data)[1] == 0x00 && (*Data)[2] == 0x01))
        (*Data)++;
  return ((*Data)[0] == 0x00 && (*Data)[1] == 0x00 && (*Data)[2] == 0x01);
}

inline int cDVDplayBuffer::GetPacketType(const uchar *Data)
{
  return Data[3];
}

inline int cDVDplayBuffer::GetStuffingLen(const uchar *Data)
{
  return Data[13] & 0x07;
}

inline int cDVDplayBuffer::GetPacketLength(const uchar *Data)
{
  return (Data[4] << 8) + Data[5] + 6;
}

inline int cDVDplayBuffer::GetPESHeaderLength(const uchar *Data)
{
  return (Data[8]);
}

int cDVDplayBuffer::ScanVideoPacket(const uchar *Data, int Count, uchar *PictureType)
{
  // Scans the video packet starting at Offset and returns its length.
  // If the return value is -1 the packet was not completely in the buffer.

  int Length = GetPacketLength(Data);
  if (Length > 0 && Length <= Count) {
     int i = 8; // the minimum length of the video packet header
     i += Data[i] + 1;   // possible additional header bytes
     for (; i < Length; i++) {
         if (Data[i] == 0 && Data[i + 1] == 0 && Data[i + 2] == 1) {
            switch (Data[i + 3]) {
              case SC_PICTURE: *PictureType = (uchar)(Data[i + 5] >> 3) & 0x07;
                               return Length;
              }
            }
         }
     PictureType = NO_PICTURE;
     return Length;
     }
  return -1;
}

#define SYSTEM_HEADER    0xBB
#define PROG_STREAM_MAP  0xBC
#ifndef PRIVATE_STREAM1
#define PRIVATE_STREAM1  0xBD
#endif
#define PADDING_STREAM   0xBE
#ifndef PRIVATE_STREAM2
#define PRIVATE_STREAM2  0xBF
#endif
#define AUDIO_STREAM_S   0xC0
#define AUDIO_STREAM_E   0xDF
#define VIDEO_STREAM_S   0xE0
#define VIDEO_STREAM_E   0xEF
#define ECM_STREAM       0xF0
#define EMM_STREAM       0xF1
#define DSM_CC_STREAM    0xF2
#define ISO13522_STREAM  0xF3
#define PROG_STREAM_DIR  0xFF

// data=PCM samples, 16 bit, LSB first, 48kHz, stereo
int cDVDplayBuffer::SendPCM(int size)
{

#define MAXSIZE 2032

  uchar buffer[MAXSIZE + 16];
  int length = 0;
  int p_size;

  if (ac3inp == ac3outp)
     return 1;

  while (size > 0) {
        if (size >= MAXSIZE)
           p_size = MAXSIZE;
        else
           p_size = size;
        length = 10;

        while (p_size) {
             if (ac3outp != ac3inp) { // data in the buffer
                buffer[(length + 6) ^ 1] = ac3data[ac3outp]; // swab because ac3dec delivers wrong byteorder
                                                             // XXX there is no 'swab' here??? (kls)
                p_size--;
                length++;
                ac3outp = (ac3outp + 1) % AC3_BUFFER_SIZE;
                }
             else
                break;
             }

        buffer[0] = 0x00;
        buffer[1] = 0x00;
        buffer[2] = 0x01;
        buffer[3] = PRIVATE_STREAM1;

        buffer[4] = (length >> 8) & 0xff;
        buffer[5] = length & 0xff;

        buffer[6] = 0x80;
        buffer[7] = 0x00;
        buffer[8] = 0x00;

        buffer[9]  = aLPCM; // substream ID
        buffer[10] = 0x00;  // other stuff (see DVD specs), ignored by driver
        buffer[11] = 0x00;
        buffer[12] = 0x00;
        buffer[13] = 0x00;
        buffer[14] = 0x00;
        buffer[15] = 0x00;

        length += 6;

        putFrame(buffer, length);
        size -= MAXSIZE;
        }
  return 0;
}

void cDVDplayBuffer::playDecodedAC3(void)
{
  int ac3_datasize = (AC3_BUFFER_SIZE + ac3inp - ac3outp) % AC3_BUFFER_SIZE;

  if (ac3_datasize) {
     if (ac3_datasize > 1024 * 48)
        SendPCM(3096);
     else if (ac3_datasize > 1024 * 32)
        SendPCM(1536);
     else if (ac3_datasize > 1024 * 16 && !(lpcm_count % 2))
        SendPCM(1536);
     else if (ac3_datasize && !(lpcm_count % 4))
        SendPCM(1536);
     lpcm_count++;
     }
  else
     lpcm_count=0;
}

void cDVDplayBuffer::handleAC3(unsigned char *sector, int length)
{
  if (dolbyDev) {
     while (length > 0) {
           int w = fwrite(sector, 1, length , dolbyDev);
           if (w < 0) {
              LOG_ERROR;
              break;
              }
           length -= w;
           sector += w;
           }
     }
  else {
     if (ac3stat == AC3_PLAY)
        ac3_decode_data(sector, sector+length, 0, &ac3inp, &ac3outp, (char *)ac3data);
     else if (ac3stat == AC3_START) {
        ac3_decode_data(sector, sector+length, 1, &ac3inp, &ac3outp, (char *)ac3data);
        ac3stat = AC3_PLAY;
        }
     }
  //playDecodedAC3();
}

void cDVDplayBuffer::putFrame(unsigned char *sector, int length)
{
  cFrame *frame = new cFrame(sector, length);
  while (Busy() && !blockInput && !Put(frame))
        ;
}

int cDVDplayBuffer::decode_packet(unsigned char *sector, int trickMode)
{
  uchar pt = 1;
#if 0
  uchar *osect = sector;
#endif

  //make sure we got a PS packet header
  if (!PacketStart(&sector, DVD_VIDEO_LB_LEN) && GetPacketType(sector) != 0xBA) {
     esyslog(LOG_ERR, "ERROR: got unexpected packet: %x %x %x %x", sector[0], sector[1], sector[2], sector[3]);
     return -1;
     }

  int offset = 14 + GetStuffingLen(sector);
  sector += offset;
  int r = DVD_VIDEO_LB_LEN - offset;
  int datalen = r;

  sector[6] &= 0x8f;
  uchar *data = sector;

  switch (GetPacketType(sector)) {
    case VIDEO_STREAM_S ... VIDEO_STREAM_E:
         {
           ScanVideoPacket(sector, r, &pt);
           if (trickMode && pt != 1)
              return pt;
           break;
         }
    case AUDIO_STREAM_S ... AUDIO_STREAM_E: {
         // no sound in trick mode
         if (trickMode)
            return 1;
         if (audioTrack != GetPacketType(sector))
            return 5;
         break;
         }
    case PRIVATE_STREAM1:
         {
           datalen = GetPacketLength(sector);
           //skip optional Header bytes
           datalen -= GetPESHeaderLength(sector);
           data += GetPESHeaderLength(sector);
           //skip mandatory header bytes
           data += 3;
           //fallthrough is intended
         }
    case PRIVATE_STREAM2:
         {
           //FIXME: Stream1 + Stream2 is ok, but is Stream2 alone also?

           // no sound in trick mode
           if (trickMode)
              return 1;

           // skip PS header bytes
           data += 6;
           // data now points to the beginning of the payload

           if (audioTrack == *data) {
              switch (audioTrack & 0xF8) {
                case aAC3:
                     data += 4;
                     // correct a3 data lenght - FIXME: why 13 ???
                     datalen -= 13;
                     handleAC3(data, datalen);
                     break;
                case aLPCM:
                     // write(audio, sector+14 , sector[19]+(sector[18]<<8)+6);
                     putFrame(sector, GetPacketLength(sector));
                     break;
                default:
                     break;
                }
              }
           return pt;
         }
    default:
    case SYSTEM_HEADER:
    case PROG_STREAM_MAP:
         {
           esyslog(LOG_ERR, "ERROR: don't know what to do - packetType: %x", GetPacketType(sector));
           // just skip them for now,l but try to debug it
           dsyslog(LOG_INFO, "DVD: curr cell: %8x, Nr of cells: %8x", cur_cell, cur_pgc->nr_of_cells);
           dsyslog(LOG_INFO, "DVD: curr pack: %8x, last sector: %8x", cur_pack, cur_pgc->cell_playback[cur_cell].last_sector);
           dsyslog(LOG_INFO, "DVD: curr pkt:  %8x, output size: %8x", pktcnt, cur_output_size);
#if 0
           // looks like my DVD is/was brocken .......
           for (int n = 0; n <= 255; n++) {
               dsyslog(LOG_INFO, "%4x   %2x %2x %2x %2x  %2x %2x %2x %2x", n * 8,
                        osect[n * 8 + 0], osect[n * 8 + 1], osect[n * 8 + 2], osect[n * 8 + 3],
                        osect[n * 8 + 4], osect[n * 8 + 5], osect[n * 8 + 6], osect[n * 8 + 7]);
               }
           return 0;
#endif
           return pt;
         }
    }
  putFrame(sector, r);
  if ((audioTrack & 0xF8) == aAC3)
     playDecodedAC3();
  return pt;
}

void cDVDplayBuffer::Empty(bool Block)
{
  if (!(blockInput || blockOutput)) {
     cPlayBuffer::Empty(true);
     ac3stat = AC3_START;
     ac3outp = ac3inp;
     }
  if (!Block)
     cPlayBuffer::Empty(false);
}

void cDVDplayBuffer::Close(void)
{
  dvd->Close();
}

int cDVDplayBuffer::SkipFrames(int Frames)
{
  return -1;
}

/* Figure out the correct pgN from the cell and update state. */
void cDVDplayBuffer::setChapid(void)
{
  int new_pgN = 0;

  while (new_pgN < cur_pgc->nr_of_programs && cur_cell >= cur_pgc->program_map[new_pgN])
        new_pgN++;

  if (new_pgN == cur_pgc->nr_of_programs) { /* We are at the last program */
     if (cur_cell > cur_pgc->nr_of_cells)
        chapid = 1; /* We are past the last cell */
     }

  chapid = new_pgN;
}

void cDVDplayBuffer::SkipSeconds(int Seconds)
{
  if (Seconds) {
     setChapid();
     int newchapid = Seconds > 0 ? chapid + 1 : chapid - 1;

     if (newchapid >= 0 && newchapid < tt_srpt->title[titleid].nr_of_ptts) {
        Empty(true);
        chapid = newchapid;
        NextState(cOPENCHAPTER);
        if (ac3stat != AC3_STOP)
           ac3stat = AC3_START;
        ac3outp = ac3inp;
        Empty(false);
        Play();
        }
     }
}

void cDVDplayBuffer::Goto(int Index, bool Still)
{
}

void cDVDplayBuffer::GetIndex(int &Current, int &Total, bool SnapToIFrame)
{
  Current = Total = -1;
}
#endif //DVDSUPPORT

// --- cTransferBuffer -------------------------------------------------------

class cTransferBuffer : public cRingBufferLinear {
private:
  cDvbApi *dvbApi;
  int fromDevice, toDevice;
  bool gotBufferReserve;
  cRemux remux;
protected:
  virtual void Input(void);
  virtual void Output(void);
public:
  cTransferBuffer(cDvbApi *DvbApi, int ToDevice, int VPid, int APid);
  virtual ~cTransferBuffer();
  void SetAudioPid(int APid);
  };

cTransferBuffer::cTransferBuffer(cDvbApi *DvbApi, int ToDevice, int VPid, int APid)
:cRingBufferLinear(VIDEOBUFSIZE, true)
,remux(VPid, APid, 0, 0, 0)
{
  dvbApi = DvbApi;
  fromDevice = dvbApi->SetModeRecord();
  toDevice = ToDevice;
  gotBufferReserve = false;
  Start();
}

cTransferBuffer::~cTransferBuffer()
{
  Stop();
  dvbApi->SetModeNormal(true);
}

void cTransferBuffer::SetAudioPid(int APid)
{
  Clear();
  //XXX we may need to have access to the audio device, too, in order to clear it
  CHECK(ioctl(toDevice, VIDEO_CLEAR_BUFFER));
  gotBufferReserve = false;
  remux.SetAudioPid(APid);
}

void cTransferBuffer::Input(void)
{
  dsyslog(LOG_INFO, "input thread started (pid=%d)", getpid());

  uchar b[MINVIDEODATA];
  int n = 0;
  while (Busy()) {
        cFile::FileReady(fromDevice, 100);
        int r = read(fromDevice, b + n, sizeof(b) - n);
        if (r > 0) {
           n += r;
           int Count = n, Result;
           const uchar *p = remux.Process(b, Count, Result);
           if (p) {
              while (Result > 0 && Busy()) {
                    int w = Put(p, Result);
                    p += w;
                    Result -= w;
                    }
              }
           if (Count > 0) {
              n -= Count;
              memmove(b, b + Count, n);
              }
           }
        else if (r < 0) {
           if (errno != EAGAIN) {
              LOG_ERROR;
              if (errno != EBUFFEROVERFLOW)
                 break;
              }
           }
        }

  dsyslog(LOG_INFO, "input thread ended (pid=%d)", getpid());
}

void cTransferBuffer::Output(void)
{
  dsyslog(LOG_INFO, "output thread started (pid=%d)", getpid());

  uchar b[MINVIDEODATA];
  while (Busy()) {
        if (!gotBufferReserve) {
           if (Available() < MAXFRAMESIZE) {
              usleep(100000); // allow the buffer to collect some reserve
              continue;
              }
           else
              gotBufferReserve = true;
           }
        int r = Get(b, sizeof(b));
        if (r > 0) {
           uchar *p = b;
           while (r > 0 && Busy()) {
                 int w = write(toDevice, p, r);
                 if (w > 0) {
                    p += w;
                    r -= w;
                    }
                 else if (w < 0 && errno != EAGAIN) {
                    LOG_ERROR;
                    Stop();
                    return;
                    }
                 }
           }
        else
           usleep(1); // this keeps the CPU load low
        }

  dsyslog(LOG_INFO, "output thread ended (pid=%d)", getpid());
}

// --- cCuttingBuffer --------------------------------------------------------

class cCuttingBuffer : public cThread {
private:
  bool active;
  int fromFile, toFile;
  cFileName *fromFileName, *toFileName;
  cIndexFile *fromIndex, *toIndex;
  cMarks fromMarks, toMarks;
protected:
  virtual void Action(void);
public:
  cCuttingBuffer(const char *FromFileName, const char *ToFileName);
  virtual ~cCuttingBuffer();
  };

cCuttingBuffer::cCuttingBuffer(const char *FromFileName, const char *ToFileName)
{
  active = false;
  fromFile = toFile = -1;
  fromFileName = toFileName = NULL;
  fromIndex = toIndex = NULL;
  if (fromMarks.Load(FromFileName) && fromMarks.Count()) {
     fromFileName = new cFileName(FromFileName, false, true);
     toFileName = new cFileName(ToFileName, true, true);
     fromIndex = new cIndexFile(FromFileName, false);
     toIndex = new cIndexFile(ToFileName, true);
     toMarks.Load(ToFileName); // doesn't actually load marks, just sets the file name
     Start();
     }
  else
     esyslog(LOG_ERR, "no editing marks found for %s", FromFileName);
}

cCuttingBuffer::~cCuttingBuffer()
{
  active = false;
  Cancel(3);
  delete fromFileName;
  delete toFileName;
  delete fromIndex;
  delete toIndex;
}

void cCuttingBuffer::Action(void)
{
  dsyslog(LOG_INFO, "video cutting thread started (pid=%d)", getpid());

  cMark *Mark = fromMarks.First();
  if (Mark) {
     fromFile = fromFileName->Open();
     toFile = toFileName->Open();
     active = fromFile >= 0 && toFile >= 0;
     int Index = Mark->position;
     Mark = fromMarks.Next(Mark);
     int FileSize = 0;
     int CurrentFileNumber = 0;
     int LastIFrame = 0;
     toMarks.Add(0);
     toMarks.Save();
     uchar buffer[MAXFRAMESIZE];
     while (active) {
           uchar FileNumber;
           int FileOffset, Length;
           uchar PictureType;

           // Read one frame:

           if (fromIndex->Get(Index++, &FileNumber, &FileOffset, &PictureType, &Length)) {
              if (FileNumber != CurrentFileNumber) {
                 fromFile = fromFileName->SetOffset(FileNumber, FileOffset);
                 CurrentFileNumber = FileNumber;
                 }
              if (fromFile >= 0) {
                 Length = ReadFrame(fromFile, buffer,  Length, sizeof(buffer));
                 if (Length < 0)
                    break;
                 }
              else
                 break;
              }
           else
              break;

           // Write one frame:

           if (PictureType == I_FRAME) { // every file shall start with an I_FRAME
              if (FileSize > MAXVIDEOFILESIZE) {
                 toFile = toFileName->NextFile();
                 if (toFile < 0)
                    break;
                 FileSize = 0;
                 }
              LastIFrame = 0;
              }
           write(toFile, buffer, Length);
           toIndex->Write(PictureType, toFileName->Number(), FileSize);
           FileSize += Length;
           if (!LastIFrame)
              LastIFrame = toIndex->Last();

           // Check editing marks:

           if (Mark && Index >= Mark->position) {
              Mark = fromMarks.Next(Mark);
              if (Mark) {
                 Index = Mark->position;
                 Mark = fromMarks.Next(Mark);
                 CurrentFileNumber = 0; // triggers SetOffset before reading next frame
                 toMarks.Add(LastIFrame);
                 toMarks.Add(toIndex->Last() + 1);
                 toMarks.Save();
                 }
              else
                 break; // final end mark reached
              }
           }
     }
  else
     esyslog(LOG_ERR, "no editing marks found!");
  dsyslog(LOG_INFO, "end video cutting thread");
}

// --- cVideoCutter ----------------------------------------------------------

cCuttingBuffer *cVideoCutter::cuttingBuffer = NULL;

bool cVideoCutter::Start(const char *FileName)
{
  if (!cuttingBuffer) {
     const char *EditedVersionName = PrefixVideoFileName(FileName, '%');
     if (EditedVersionName && RemoveVideoFile(EditedVersionName) && MakeDirs(EditedVersionName, true)) {
        cuttingBuffer = new cCuttingBuffer(FileName, EditedVersionName);
        return true;
        }
     }
  return false;
}

void cVideoCutter::Stop(void)
{
  delete cuttingBuffer;
  cuttingBuffer = NULL;
}

bool cVideoCutter::Active(void)
{
  if (cuttingBuffer) {
     if (cuttingBuffer->Active())
        return true;
     Stop();
     }
  return false;
}

// --- cDvbApi ---------------------------------------------------------------


static const char *OstName(const char *Name, int n)
{
  static char buffer[_POSIX_PATH_MAX];
  snprintf(buffer, sizeof(buffer), "%s%d", Name, n);
  return buffer;
}

static int OstOpen(const char *Name, int n, int Mode, bool ReportError = false)
{
  const char *FileName = OstName(Name, n);
  int fd = open(FileName, Mode);
  if (fd < 0 && ReportError)
     LOG_ERROR_STR(FileName);
  return fd;
}

int cDvbApi::NumDvbApis = 0;
int cDvbApi::useDvbApi = 0;
cDvbApi *cDvbApi::dvbApi[MAXDVBAPI] = { NULL };
cDvbApi *cDvbApi::PrimaryDvbApi = NULL;
char *cDvbApi::audioCommand = NULL;

cDvbApi::cDvbApi(int n)
{
  vPid = aPid1 = aPid2 = dPid1 = dPid2 = 0;
  siProcessor = NULL;
  recordBuffer = NULL;
  replayBuffer = NULL;
  transferBuffer = NULL;
  transferringFromDvbApi = NULL;
  ca = 0;
  priority = -1;

  // Devices that are only present on DVB-C or DVB-S cards:

  fd_qamfe   = OstOpen(DEV_OST_QAMFE,  n, O_RDWR);
  fd_qpskfe  = OstOpen(DEV_OST_QPSKFE, n, O_RDWR);
  fd_sec     = OstOpen(DEV_OST_SEC,    n, O_RDWR);

  // Devices that all DVB cards must have:

  fd_demuxv  = OstOpen(DEV_OST_DEMUX,  n, O_RDWR | O_NONBLOCK, true);
  fd_demuxa1 = OstOpen(DEV_OST_DEMUX,  n, O_RDWR | O_NONBLOCK, true);
  fd_demuxa2 = OstOpen(DEV_OST_DEMUX,  n, O_RDWR | O_NONBLOCK, true);
  fd_demuxd1 = OstOpen(DEV_OST_DEMUX,  n, O_RDWR | O_NONBLOCK, true);
  fd_demuxd2 = OstOpen(DEV_OST_DEMUX,  n, O_RDWR | O_NONBLOCK, true);
  fd_demuxt  = OstOpen(DEV_OST_DEMUX,  n, O_RDWR | O_NONBLOCK, true);

  // Devices not present on "budget" cards:

  fd_osd     = OstOpen(DEV_OST_OSD,    n, O_RDWR);
  fd_video   = OstOpen(DEV_OST_VIDEO,  n, O_RDWR | O_NONBLOCK);
  fd_audio   = OstOpen(DEV_OST_AUDIO,  n, O_RDWR | O_NONBLOCK);

  // Devices that may not be available, and are not necessary for normal operation:

  videoDev   = OstOpen(DEV_VIDEO,      n, O_RDWR);

  // Devices that will be dynamically opened and closed when necessary:

  fd_dvr     = -1;

  // Video format:

  SetVideoFormat(Setup.VideoFormat ? VIDEO_FORMAT_16_9 : VIDEO_FORMAT_4_3);

  // We only check the devices that must be present - the others will be checked before accessing them:

  if (((fd_qpskfe >= 0 && fd_sec >= 0) || fd_qamfe >= 0) && fd_demuxv >= 0 && fd_demuxa1 >= 0 && fd_demuxa2 >= 0 && fd_demuxd1 >= 0 && fd_demuxd2 >= 0 && fd_demuxt >= 0) {
     siProcessor = new cSIProcessor(OstName(DEV_OST_DEMUX, n));
     if (!dvbApi[0]) // only the first one shall set the system time
        siProcessor->SetUseTSTime(Setup.SetSystemTime);
     }
  else
     esyslog(LOG_ERR, "ERROR: can't open video device %d", n);
  cols = rows = 0;

  ovlGeoSet = ovlStat = ovlFbSet = false;
  ovlBrightness = ovlColour = ovlHue = ovlContrast = 32768;
  ovlClipCount = 0;

#if defined(DEBUG_OSD) || defined(REMOTE_KBD)
  initscr();
  keypad(stdscr, true);
  nonl();
  cbreak();
  noecho();
  timeout(10);
#endif
#if defined(DEBUG_OSD)
  memset(&colorPairs, 0, sizeof(colorPairs));
  start_color();
  leaveok(stdscr, true);
  window = NULL;
#else
  osd = NULL;
#endif
  currentChannel = 1;
}

cDvbApi::~cDvbApi()
{
  delete siProcessor;
  Close();
  StopReplay();
  StopRecord();
  StopTransfer();
  OvlO(false); //Overlay off!
  // We're not explicitly closing any device files here, since this sometimes
  // caused segfaults. Besides, the program is about to terminate anyway...
#if defined(DEBUG_OSD) || defined(REMOTE_KBD)
  endwin();
#endif
}

void cDvbApi::SetUseDvbApi(int n)
{
  if (n < MAXDVBAPI)
     useDvbApi |= (1 << n);
}

bool cDvbApi::SetPrimaryDvbApi(int n)
{
  n--;
  if (0 <= n && n < NumDvbApis && dvbApi[n]) {
     isyslog(LOG_INFO, "setting primary DVB to %d", n + 1);
     PrimaryDvbApi = dvbApi[n];
     return true;
     }
  esyslog(LOG_ERR, "invalid DVB interface: %d", n + 1);
  return false;
}

cDvbApi *cDvbApi::GetDvbApi(int Ca, int Priority)
{
  cDvbApi *d = NULL, *dMinPriority = NULL;
  int index = Ca - 1;
  for (int i = 0; i < MAXDVBAPI; i++) {
      if (dvbApi[i]) {
         if (i == index) { // means we need exactly _this_ device
            d = dvbApi[i];
            break;
            }
         else if (Ca == 0) { // means any device would be acceptable
            if (!d || !dvbApi[i]->Recording() || (d->Recording() && d->Priority() > dvbApi[i]->Priority()))
               d = dvbApi[i]; // this is one that is either not currently recording or has the lowest priority
            if (d && d != PrimaryDvbApi && !d->Recording()) // avoids the PrimaryDvbApi if possible
               break;
            if (d && d->Recording() && d->Priority() < Setup.PrimaryLimit && (!dMinPriority || d->Priority() < dMinPriority->Priority()))
               dMinPriority = d; // this is the one with the lowest priority below Setup.PrimaryLimit
            }
         }
      }
  if (d == PrimaryDvbApi) { // the PrimaryDvbApi was the only one that was free
     if (Priority < Setup.PrimaryLimit)
        return NULL;        // not enough priority to use the PrimaryDvbApi
     if (dMinPriority)      // there's one that must not use the PrimaryDvbApi...
        d = dMinPriority;   // ...so let's kick out that one
     }
  return (d                           // we found one...
      && (!d->Recording()             // ...that's either not currently recording...
          || d->Priority() < Priority // ...or has a lower priority...
          || (!d->Ca() && Ca)))       // ...or doesn't need this card
          ? d : NULL;
}

int cDvbApi::Index(void)
{
  for (int i = 0; i < MAXDVBAPI; i++) {
      if (dvbApi[i] == this)
         return i;
      }
  return -1;
}

bool cDvbApi::Probe(const char *FileName)
{
  if (access(FileName, F_OK) == 0) {
     dsyslog(LOG_INFO, "probing %s", FileName);
     int f = open(FileName, O_RDONLY);
     if (f >= 0) {
        close(f);
        return true;
        }
     else if (errno != ENODEV && errno != EINVAL)
        LOG_ERROR_STR(FileName);
     }
  else if (errno != ENOENT)
     LOG_ERROR_STR(FileName);
  return false;
}

bool cDvbApi::Init(void)
{
  NumDvbApis = 0;
  for (int i = 0; i < MAXDVBAPI; i++) {
      if (useDvbApi == 0 || (useDvbApi & (1 << i)) != 0) {
         if (Probe(OstName(DEV_OST_QPSKFE, i)) || Probe(OstName(DEV_OST_QAMFE, i)))
            dvbApi[NumDvbApis++] = new cDvbApi(i);
         else
            break;
         }
      }
  PrimaryDvbApi = dvbApi[0];
  if (NumDvbApis > 0) {
     isyslog(LOG_INFO, "found %d video device%s", NumDvbApis, NumDvbApis > 1 ? "s" : "");
     } // need braces because of isyslog-macro
  else {
     esyslog(LOG_ERR, "ERROR: no video device found, giving up!");
     }
  return NumDvbApis > 0;
}

void cDvbApi::Cleanup(void)
{
  for (int i = 0; i < MAXDVBAPI; i++) {
      delete dvbApi[i];
      dvbApi[i] = NULL;
      }
  PrimaryDvbApi = NULL;
}

const cSchedules *cDvbApi::Schedules(cThreadLock *ThreadLock) const
{
  if (siProcessor && ThreadLock->Lock(siProcessor))
     return siProcessor->Schedules();
  return NULL;
}

bool cDvbApi::GrabImage(const char *FileName, bool Jpeg, int Quality, int SizeX, int SizeY)
{
  if (videoDev < 0)
     return false;
  int result = 0;
  // just do this once?
  struct video_mbuf mbuf;
  result |= ioctl(videoDev, VIDIOCGMBUF, &mbuf);
  int msize = mbuf.size;
  // gf: this needs to be a protected member of cDvbApi! //XXX kls: WHY???
  unsigned char *mem = (unsigned char *)mmap(0, msize, PROT_READ | PROT_WRITE, MAP_SHARED, videoDev, 0);
  if (!mem || mem == (unsigned char *)-1)
     return false;
  // set up the size and RGB
  struct video_capability vc;
  result |= ioctl(videoDev, VIDIOCGCAP, &vc);
  struct video_mmap vm;
  vm.frame = 0;
  if ((SizeX > 0) && (SizeX <= vc.maxwidth) &&
      (SizeY > 0) && (SizeY <= vc.maxheight)) {
     vm.width = SizeX;
     vm.height = SizeY;
     }
  else {
     vm.width = vc.maxwidth;
     vm.height = vc.maxheight;
     }
  vm.format = VIDEO_PALETTE_RGB24;
  // this needs to be done every time:
  result |= ioctl(videoDev, VIDIOCMCAPTURE, &vm);
  result |= ioctl(videoDev, VIDIOCSYNC, &vm.frame);
  // make RGB out of BGR:
  int memsize = vm.width * vm.height;
  unsigned char *mem1 = mem;
  for (int i = 0; i < memsize; i++) {
      unsigned char tmp = mem1[2];
      mem1[2] = mem1[0];
      mem1[0] = tmp;
      mem1 += 3;
      }

  if (Quality < 0)
     Quality = 255; //XXX is this 'best'???

  isyslog(LOG_INFO, "grabbing to %s (%s %d %d %d)", FileName, Jpeg ? "JPEG" : "PNM", Quality, vm.width, vm.height);
  FILE *f = fopen(FileName, "wb");
  if (f) {
     if (Jpeg) {
        // write JPEG file:
        struct jpeg_compress_struct cinfo;
        struct jpeg_error_mgr jerr;
        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_compress(&cinfo);
        jpeg_stdio_dest(&cinfo, f);
        cinfo.image_width = vm.width;
        cinfo.image_height = vm.height;
        cinfo.input_components = 3;
        cinfo.in_color_space = JCS_RGB;

        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, Quality, true);
        jpeg_start_compress(&cinfo, true);

        int rs = vm.width * 3;
        JSAMPROW rp[vm.height];
        for (int k = 0; k < vm.height; k++)
            rp[k] = &mem[rs * k];
        jpeg_write_scanlines(&cinfo, rp, vm.height);
        jpeg_finish_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);
        }
     else {
        // write PNM file:
        if (fprintf(f, "P6\n%d\n%d\n255\n", vm.width, vm.height) < 0 ||
            fwrite(mem, vm.width * vm.height * 3, 1, f) < 0) {
           LOG_ERROR_STR(FileName);
           result |= 1;
           }
        }
     fclose(f);
     }
  else {
     LOG_ERROR_STR(FileName);
     result |= 1;
     }

  if (ovlStat && ovlGeoSet) {
     // switch the Overlay on again (gf: why have i to do anything again?)
     OvlG(ovlSizeX, ovlSizeY, ovlPosX, ovlPosY);
     }
  if (ovlFbSet)
     OvlP(ovlBrightness, ovlColour, ovlHue, ovlContrast);

  munmap(mem, msize);
  return result == 0;
}

bool cDvbApi::OvlF(int SizeX, int SizeY, int FbAddr, int Bpp, int Palette)
{
  if (videoDev < 0)
     return false;
  int result = 0;
  // get the actual X-Server settings???
  // plausibility-check problem: can't be verified w/o X-server!!!
  if (SizeX <= 0 || SizeY <= 0 || FbAddr == 0 || Bpp / 8 > 4 ||
      Bpp / 8 <= 0 || Palette <= 0 || Palette > 13 || ovlClipCount < 0 ||
      SizeX > 4096 || SizeY > 4096) {
     ovlFbSet = ovlGeoSet = false;
     OvlO(false);
     return false;
     }
  else {
    dsyslog(LOG_INFO, "OvlF: %d %d %x %d %d", SizeX, SizeY, FbAddr, Bpp, Palette);
    // this is the problematic part!
    struct video_buffer vb;
    result |= ioctl(videoDev, VIDIOCGFBUF, &vb);
    vb.base = (void*)FbAddr;
    vb.depth = Bpp;
    vb.height = SizeY;
    vb.width = SizeX;
    vb.bytesperline = ((vb.depth + 1) / 8) * vb.width;
    //now the real thing: setting the framebuffer
    result |= ioctl(videoDev, VIDIOCSFBUF, &vb);
    if (result) {
       ovlFbSet = ovlGeoSet = false;
       ovlClipCount = 0;
       OvlO(false);
       return false;
       }
    else {
       ovlFbSizeX = SizeX;
       ovlFbSizeY = SizeY;
       ovlBpp = Bpp;
       ovlPalette = Palette;
       ovlFbSet = true;
       return true;
      }
    }
}

bool cDvbApi::OvlG(int SizeX, int SizeY, int PosX, int PosY)
{
  if (videoDev < 0)
     return false;
  int result = 0;
  // get the actual X-Server settings???
  struct video_capability vc;
  result |= ioctl(videoDev, VIDIOCGCAP, &vc);
  if (!ovlFbSet)
     return false;
  if (SizeX < vc.minwidth || SizeY < vc.minheight ||
      SizeX > vc.maxwidth || SizeY>vc.maxheight
//      || PosX > FbSizeX || PosY > FbSizeY
//         PosX < -SizeX || PosY < -SizeY ||
     ) {
     ovlGeoSet = false;
     OvlO(false);
     return false;
     }
  else {
     struct video_window vw;
     result |= ioctl(videoDev, VIDIOCGWIN,  &vw);
     vw.x = PosX;
     vw.y = PosY;
     vw.width = SizeX;
     vw.height = SizeY;
     vw.chromakey = ovlPalette;
     vw.flags = VIDEO_WINDOW_CHROMAKEY; // VIDEO_WINDOW_INTERLACE; //VIDEO_CLIP_BITMAP;
     vw.clips = ovlClipRects;
     vw.clipcount = ovlClipCount;
     result |= ioctl(videoDev, VIDIOCSWIN, &vw);
     if (result) {
        ovlGeoSet = false;
        ovlClipCount = 0;
        return false;
        }
     else {
        ovlSizeX = SizeX;
        ovlSizeY = SizeY;
        ovlPosX = PosX;
        ovlPosY = PosY;
        ovlGeoSet = true;
        ovlStat = true;
        return true;
        }
     }
}

bool cDvbApi::OvlC(int ClipCount, CRect *cr)
{
  if (videoDev < 0)
     return false;
  if (ovlGeoSet && ovlFbSet) {
     for (int i = 0; i < ClipCount; i++) {
         ovlClipRects[i].x = cr[i].x;
         ovlClipRects[i].y = cr[i].y;
         ovlClipRects[i].width = cr[i].width;
         ovlClipRects[i].height = cr[i].height;
         ovlClipRects[i].next = &(ovlClipRects[i + 1]);
         }
     ovlClipCount = ClipCount;
     //use it:
     return OvlG(ovlSizeX, ovlSizeY, ovlPosX, ovlPosY);
     }
  return false;
}

bool cDvbApi::OvlP(__u16 Brightness, __u16 Colour, __u16 Hue, __u16 Contrast)
{
  if (videoDev < 0)
     return false;
  int result = 0;
  ovlBrightness = Brightness;
  ovlColour = Colour;
  ovlHue = Hue;
  ovlContrast = Contrast;
  struct video_picture vp;
  if (!ovlFbSet)
     return false;
  result |= ioctl(videoDev, VIDIOCGPICT, &vp);
  vp.brightness = Brightness;
  vp.colour = Colour;
  vp.hue = Hue;
  vp.contrast = Contrast;
  vp.depth = ovlBpp;
  vp.palette = ovlPalette; // gf: is this always ok? VIDEO_PALETTE_RGB565;
  result |= ioctl(videoDev, VIDIOCSPICT, &vp);
  return result == 0;
}

bool cDvbApi::OvlO(bool Value)
{
  if (videoDev < 0)
     return false;
  int result = 0;
  if (!ovlGeoSet && Value)
     return false;
  int one = 1;
  int zero = 0;
  result |= ioctl(videoDev, VIDIOCCAPTURE, Value ? &one : &zero);
  ovlStat = Value;
  if (result) {
     ovlStat = false;
     return false;
     }
  return true;
}

#ifdef DEBUG_OSD
void cDvbApi::SetColor(eDvbColor colorFg, eDvbColor colorBg)
{
  int color = (colorBg << 16) | colorFg | 0x80000000;
  for (int i = 0; i < MaxColorPairs; i++) {
      if (!colorPairs[i]) {
         colorPairs[i] = color;
         init_pair(i + 1, colorFg, colorBg);
         wattrset(window, COLOR_PAIR(i + 1));
         break;
         }
      else if (color == colorPairs[i]) {
         wattrset(window, COLOR_PAIR(i + 1));
         break;
         }
      }
}
#endif

void cDvbApi::Open(int w, int h)
{
  int d = (h < 0) ? Setup.OSDheight + h : 0;
  h = abs(h);
  cols = w;
  rows = h;
#ifdef DEBUG_OSD
  window = subwin(stdscr, h, w, d, 0);
  syncok(window, true);
  #define B2C(b) (((b) * 1000) / 255)
  #define SETCOLOR(n, r, g, b, o) init_color(n, B2C(r), B2C(g), B2C(b))
  //XXX
  SETCOLOR(clrBackground,  0x00, 0x00, 0x00, 127); // background 50% gray
  SETCOLOR(clrBlack,       0x00, 0x00, 0x00, 255);
  SETCOLOR(clrRed,         0xFC, 0x14, 0x14, 255);
  SETCOLOR(clrGreen,       0x24, 0xFC, 0x24, 255);
  SETCOLOR(clrYellow,      0xFC, 0xC0, 0x24, 255);
  SETCOLOR(clrBlue,        0x00, 0x00, 0xFC, 255);
  SETCOLOR(clrCyan,        0x00, 0xFC, 0xFC, 255);
  SETCOLOR(clrMagenta,     0xB0, 0x00, 0xFC, 255);
  SETCOLOR(clrWhite,       0xFC, 0xFC, 0xFC, 255);
#else
  w *= charWidth;
  h *= lineHeight;
  d *= lineHeight;
  int x = (720 - (Setup.OSDwidth - 1) * charWidth) / 2; //TODO PAL vs. NTSC???
  int y = (576 - Setup.OSDheight * lineHeight) / 2 + d;
  //XXX
  osd = new cDvbOsd(fd_osd, x, y);
  //XXX TODO this should be transferred to the places where the individual windows are requested (there's too much detailed knowledge here!)
  if (h / lineHeight == 5) { //XXX channel display
     osd->Create(0,              0, w, h, 4);
     }
  else if (h / lineHeight == 1) { //XXX info display
     osd->Create(0,              0, w, h, 4);
     }
  else if (d == 0) { //XXX full menu
     osd->Create(0,                            0, w,                         lineHeight, 2);
     osd->Create(0,                   lineHeight, w, (Setup.OSDheight - 3) * lineHeight, 2, true, clrBackground, clrCyan, clrWhite, clrBlack);
     osd->Create(0, (Setup.OSDheight - 2) * lineHeight, w,               2 * lineHeight, 4);
     }
  else { //XXX progress display
     /*XXX
     osd->Create(0,              0, w, lineHeight, 1);
     osd->Create(0,     lineHeight, w, lineHeight, 2, false);
     osd->Create(0, 2 * lineHeight, w, lineHeight, 1);
     XXX*///XXX some pixels are not drawn correctly with lower bpp values
     osd->Create(0,              0, w, 3*lineHeight, 4);
     }
#endif
}

void cDvbApi::Close(void)
{
#ifdef DEBUG_OSD
  if (window) {
     delwin(window);
     window = 0;
     }
#else
  delete osd;
  osd = NULL;
#endif
}

void cDvbApi::Clear(void)
{
#ifdef DEBUG_OSD
  SetColor(clrBackground, clrBackground);
  Fill(0, 0, cols, rows, clrBackground);
#else
  osd->Clear();
#endif
}

void cDvbApi::Fill(int x, int y, int w, int h, eDvbColor color)
{
  if (x < 0) x = cols + x;
  if (y < 0) y = rows + y;
#ifdef DEBUG_OSD
  SetColor(color, color);
  for (int r = 0; r < h; r++) {
      wmove(window, y + r, x); // ncurses wants 'y' before 'x'!
      whline(window, ' ', w);
      }
  wsyncup(window); // shouldn't be necessary because of 'syncok()', but w/o it doesn't work
#else
  osd->Fill(x * charWidth, y * lineHeight, (x + w) * charWidth - 1, (y + h) * lineHeight - 1, color);
#endif
}

void cDvbApi::SetBitmap(int x, int y, const cBitmap &Bitmap)
{
#ifndef DEBUG_OSD
  osd->SetBitmap(x, y, Bitmap);
#endif
}

void cDvbApi::ClrEol(int x, int y, eDvbColor color)
{
  Fill(x, y, cols - x, 1, color);
}

int cDvbApi::CellWidth(void)
{
#ifdef DEBUG_OSD
  return 1;
#else
  return charWidth;
#endif
}

int cDvbApi::LineHeight(void)
{
#ifdef DEBUG_OSD
  return 1;
#else
  return lineHeight;
#endif
}

int cDvbApi::Width(unsigned char c)
{
#ifdef DEBUG_OSD
  return 1;
#else
  return osd->Width(c);
#endif
}

int cDvbApi::WidthInCells(const char *s)
{
#ifdef DEBUG_OSD
  return strlen(s);
#else
  return (osd->Width(s) + charWidth - 1) / charWidth;
#endif
}

eDvbFont cDvbApi::SetFont(eDvbFont Font)
{
#ifdef DEBUG_OSD
  return Font;
#else
  return osd->SetFont(Font);
#endif
}

void cDvbApi::Text(int x, int y, const char *s, eDvbColor colorFg, eDvbColor colorBg)
{
  if (x < 0) x = cols + x;
  if (y < 0) y = rows + y;
#ifdef DEBUG_OSD
  SetColor(colorFg, colorBg);
  wmove(window, y, x); // ncurses wants 'y' before 'x'!
  waddnstr(window, s, cols - x);
#else
  osd->Text(x * charWidth, y * lineHeight, s, colorFg, colorBg);
#endif
}

void cDvbApi::Flush(void)
{
#ifndef DEBUG_OSD
  if (osd)
     osd->Flush();
#endif
}

int cDvbApi::SetModeRecord(void)
{
  // Sets up the DVB device for recording

  SetPids(true);
  if (fd_dvr >= 0)
     close(fd_dvr);
  fd_dvr = OstOpen(DEV_OST_DVR, Index(), O_RDONLY | O_NONBLOCK);
  if (fd_dvr < 0)
     LOG_ERROR;
  return fd_dvr;
}

void cDvbApi::SetModeReplay(void)
{
  // Sets up the DVB device for replay

  if (fd_video >= 0 && fd_audio >= 0) {
     if (siProcessor)
        siProcessor->SetStatus(false);
     CHECK(ioctl(fd_video, VIDEO_SET_BLANK, true));
     CHECK(ioctl(fd_audio, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_MEMORY));
     CHECK(ioctl(fd_audio, AUDIO_SET_AV_SYNC, true));
     CHECK(ioctl(fd_audio, AUDIO_PLAY));
     CHECK(ioctl(fd_video, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_MEMORY));
     CHECK(ioctl(fd_video, VIDEO_PLAY));
     }
}

void cDvbApi::SetModeNormal(bool FromRecording)
{
  // Puts the DVB device back into "normal" viewing mode (after replay or recording)

  if (FromRecording) {
     close(fd_dvr);
     fd_dvr = -1;
     SetPids(false);
     }
  else {
     if (fd_video >= 0 && fd_audio >= 0) {
        CHECK(ioctl(fd_video, VIDEO_STOP, true));
        CHECK(ioctl(fd_audio, AUDIO_STOP, true));
        CHECK(ioctl(fd_video, VIDEO_CLEAR_BUFFER));
        CHECK(ioctl(fd_audio, AUDIO_CLEAR_BUFFER));
        CHECK(ioctl(fd_video, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX));
        CHECK(ioctl(fd_audio, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_DEMUX));
        CHECK(ioctl(fd_audio, AUDIO_SET_AV_SYNC, true));
        CHECK(ioctl(fd_audio, AUDIO_SET_MUTE, false));
        if (siProcessor)
           siProcessor->SetStatus(true);
        }
     }
}

void cDvbApi::SetVideoFormat(videoFormat_t Format)
{
  if (fd_video)
     CHECK(ioctl(fd_video, VIDEO_SET_FORMAT, Format));
}

bool cDvbApi::SetPid(int fd, dmxPesType_t PesType, int Pid, dmxOutput_t Output)
{
  if (Pid) {
     CHECK(ioctl(fd, DMX_STOP));
     dmxPesFilterParams pesFilterParams;
     pesFilterParams.pid     = Pid;
     pesFilterParams.input   = DMX_IN_FRONTEND;
     pesFilterParams.output  = Output;
     pesFilterParams.pesType = PesType;
     pesFilterParams.flags   = DMX_IMMEDIATE_START;
     if (ioctl(fd, DMX_SET_PES_FILTER, &pesFilterParams) < 0) {
        if (Pid != 0x1FFF)
           LOG_ERROR;
        return false;
        }
     }
  return true;
}

bool cDvbApi::SetPids(bool ForRecording)
{
  return SetVpid(vPid,   ForRecording ? DMX_OUT_TS_TAP : DMX_OUT_DECODER) &&
         SetApid1(aPid1, ForRecording ? DMX_OUT_TS_TAP : DMX_OUT_DECODER) &&
         SetApid2(ForRecording ? aPid2 : 0, DMX_OUT_TS_TAP) &&
         SetDpid1(ForRecording ? dPid1 : 0, DMX_OUT_TS_TAP) &&
         SetDpid2(ForRecording ? dPid2 : 0, DMX_OUT_TS_TAP);
}

bool cDvbApi::SetChannel(int ChannelNumber, int FrequencyMHz, char Polarization, int Diseqc, int Srate, int Vpid, int Apid1, int Apid2, int Dpid1, int Dpid2, int Tpid, int Ca, int Pnr)
{
  // Make sure the siProcessor won't access the device while switching
  cThreadLock ThreadLock(siProcessor);

  StopTransfer();
  StopReplay();

  // Must set this anyway to avoid getting stuck when switching through
  // channels with 'Up' and 'Down' keys:
  currentChannel = ChannelNumber;
  vPid = Vpid;
  aPid1 = Apid1;
  aPid2 = Apid2;
  dPid1 = Dpid1;
  dPid2 = Dpid2;

  // Avoid noise while switching:

  if (fd_video >= 0 && fd_audio >= 0) {
     CHECK(ioctl(fd_audio, AUDIO_SET_MUTE, true));
     CHECK(ioctl(fd_video, VIDEO_SET_BLANK, true));
     CHECK(ioctl(fd_video, VIDEO_CLEAR_BUFFER));
     CHECK(ioctl(fd_audio, AUDIO_CLEAR_BUFFER));
     }

  // If this card can't receive this channel, we must not actually switch
  // the channel here, because that would irritate the driver when we
  // start replaying in Transfer Mode immediately after switching the channel:
  bool NeedsTransferMode = (this == PrimaryDvbApi && Ca && Ca != Index() + 1);

  if (!NeedsTransferMode) {

     // Turn off current PIDs:

     SetVpid( 0x1FFF, DMX_OUT_DECODER);
     SetApid1(0x1FFF, DMX_OUT_DECODER);
     SetApid2(0x1FFF, DMX_OUT_DECODER);
     SetDpid1(0x1FFF, DMX_OUT_DECODER);
     SetDpid2(0x1FFF, DMX_OUT_DECODER);
     SetTpid( 0x1FFF, DMX_OUT_DECODER);

     bool ChannelSynced = false;

     if (fd_qpskfe >= 0 && fd_sec >= 0) { // DVB-S

        // Frequency offsets:

        unsigned int freq = FrequencyMHz;
        int tone = SEC_TONE_OFF;

        if (freq < (unsigned int)Setup.LnbSLOF) {
           freq -= Setup.LnbFrequLo;
           tone = SEC_TONE_OFF;
           }
        else {
           freq -= Setup.LnbFrequHi;
           tone = SEC_TONE_ON;
           }

        qpskParameters qpsk;
        qpsk.iFrequency = freq * 1000UL;
        qpsk.SymbolRate = Srate * 1000UL;
        qpsk.FEC_inner = FEC_AUTO;

        int volt = (Polarization == 'v' || Polarization == 'V') ? SEC_VOLTAGE_13 : SEC_VOLTAGE_18;

        // DiseqC:

        secCommand scmd;
        scmd.type = 0;
        scmd.u.diseqc.addr = 0x10;
        scmd.u.diseqc.cmd = 0x38;
        scmd.u.diseqc.numParams = 1;
        scmd.u.diseqc.params[0] = 0xF0 | ((Diseqc * 4) & 0x0F) | (tone == SEC_TONE_ON ? 1 : 0) | (volt == SEC_VOLTAGE_18 ? 2 : 0);

        secCmdSequence scmds;
        scmds.voltage = volt;
        scmds.miniCommand = SEC_MINI_NONE;
        scmds.continuousTone = tone;
        scmds.numCommands = Setup.DiSEqC ? 1 : 0;
        scmds.commands = &scmd;

        CHECK(ioctl(fd_sec, SEC_SEND_SEQUENCE, &scmds));

        // Tuning:

        CHECK(ioctl(fd_qpskfe, QPSK_TUNE, &qpsk));

        // Wait for channel sync:

        if (cFile::FileReady(fd_qpskfe, 5000)) {
           qpskEvent event;
           int res = ioctl(fd_qpskfe, QPSK_GET_EVENT, &event);
           if (res >= 0)
              ChannelSynced = event.type == FE_COMPLETION_EV;
           else
              esyslog(LOG_ERR, "ERROR %d in qpsk get event", res);
           }
        else
           esyslog(LOG_ERR, "ERROR: timeout while tuning");
        }
     else if (fd_qamfe >= 0) { // DVB-C

        // Frequency and symbol rate:

        qamParameters qam;
        qam.Frequency = FrequencyMHz * 1000000UL;
        qam.SymbolRate = Srate * 1000UL;
        qam.FEC_inner = FEC_AUTO;
        qam.QAM = QAM_64;

        // Tuning:

        CHECK(ioctl(fd_qamfe, QAM_TUNE, &qam));

        // Wait for channel sync:

        if (cFile::FileReady(fd_qamfe, 5000)) {
           qamEvent event;
           int res = ioctl(fd_qamfe, QAM_GET_EVENT, &event);
           if (res >= 0)
              ChannelSynced = event.type == FE_COMPLETION_EV;
           else
              esyslog(LOG_ERR, "ERROR %d in qam get event", res);
           }
        else
           esyslog(LOG_ERR, "ERROR: timeout while tuning");
        }
     else {
        esyslog(LOG_ERR, "ERROR: attempt to set channel without DVB-S or DVB-C device");
        return false;
        }

     if (!ChannelSynced) {
        esyslog(LOG_ERR, "ERROR: channel %d not sync'ed!", ChannelNumber);
        if (this == PrimaryDvbApi)
           cThread::RaisePanic();
        return false;
        }

     // PID settings:

     if (!SetPids(false)) {
        esyslog(LOG_ERR, "ERROR: failed to set PIDs for channel %d", ChannelNumber);
        return false;
        }
     SetTpid(Tpid, DMX_OUT_DECODER);
     if (fd_audio >= 0)
        CHECK(ioctl(fd_audio, AUDIO_SET_AV_SYNC, true));
     }

  if (this == PrimaryDvbApi && siProcessor)
     siProcessor->SetCurrentServiceID(Pnr);

  // If this DVB card can't receive this channel, let's see if we can
  // use the card that actually can receive it and transfer data from there:

  if (NeedsTransferMode) {
     cDvbApi *CaDvbApi = GetDvbApi(Ca, 0);
     if (CaDvbApi) {
        if (!CaDvbApi->Recording()) {
           if (CaDvbApi->SetChannel(ChannelNumber, FrequencyMHz, Polarization, Diseqc, Srate, Vpid, Apid1, Apid2, Dpid1, Dpid2, Tpid, Ca, Pnr)) {
              SetModeReplay();
              transferringFromDvbApi = CaDvbApi->StartTransfer(fd_video);
              }
           }
        }
     }

  if (fd_video >= 0 && fd_audio >= 0) {
     CHECK(ioctl(fd_audio, AUDIO_SET_MUTE, false));
     CHECK(ioctl(fd_video, VIDEO_SET_BLANK, false));
     }

  return true;
}

bool cDvbApi::Transferring(void)
{
  return transferBuffer;
}

cDvbApi *cDvbApi::StartTransfer(int TransferToVideoDev)
{
  StopTransfer();
  transferBuffer = new cTransferBuffer(this, TransferToVideoDev, vPid, aPid1);
  return this;
}

void cDvbApi::StopTransfer(void)
{
  if (transferBuffer) {
     delete transferBuffer;
     transferBuffer = NULL;
     }
  if (transferringFromDvbApi) {
     transferringFromDvbApi->StopTransfer();
     transferringFromDvbApi = NULL;
     }
}

int cDvbApi::SecondsToFrames(int Seconds)
{
  return Seconds * FRAMESPERSEC;
}

bool cDvbApi::Recording(void)
{
  if (recordBuffer && !recordBuffer->Active())
     StopRecord();
  return recordBuffer != NULL;
}

bool cDvbApi::Replaying(void)
{
  if (replayBuffer && !replayBuffer->Active())
     StopReplay();
  return replayBuffer != NULL;
}

bool cDvbApi::StartRecord(const char *FileName, int Ca, int Priority)
{
  if (Recording()) {
     esyslog(LOG_ERR, "ERROR: StartRecord() called while recording - ignored!");
     return false;
     }

  StopTransfer();

  StopReplay(); // TODO: remove this if the driver is able to do record and replay at the same time

  // Check FileName:

  if (!FileName) {
     esyslog(LOG_ERR, "ERROR: StartRecord: file name is (null)");
     return false;
     }
  isyslog(LOG_INFO, "record %s", FileName);

  // Create directories if necessary:

  if (!MakeDirs(FileName, true))
     return false;

  // Create recording buffer:

  recordBuffer = new cRecordBuffer(this, FileName, vPid, aPid1, aPid2, dPid1, dPid2);

  if (recordBuffer) {
     ca = Ca;
     priority = Priority;
     return true;
     }
  else
     esyslog(LOG_ERR, "ERROR: can't allocate recording buffer");

  return false;
}

void cDvbApi::StopRecord(void)
{
  if (recordBuffer) {
     delete recordBuffer;
     recordBuffer = NULL;
     ca = 0;
     priority = -1;
     }
}

bool cDvbApi::StartReplay(const char *FileName)
{
  if (Recording()) {
     esyslog(LOG_ERR, "ERROR: StartReplay() called while recording - ignored!");
     return false;
     }
  StopTransfer();
  StopReplay();
  if (fd_video >= 0 && fd_audio >= 0) {

     // Check FileName:

     if (!FileName) {
        esyslog(LOG_ERR, "ERROR: StartReplay: file name is (null)");
        return false;
        }
     isyslog(LOG_INFO, "replay %s", FileName);

     // Create replay buffer:

     replayBuffer = new cReplayBuffer(this, fd_video, fd_audio, FileName);
     if (replayBuffer)
        return true;
     else
        esyslog(LOG_ERR, "ERROR: can't allocate replaying buffer");
     }
  return false;
}

#ifdef DVDSUPPORT
bool cDvbApi::StartDVDplay(cDVD *dvd, int TitleID)
{
  if (Recording()) {
     esyslog(LOG_ERR, "ERROR: StartDVDplay() called while recording - ignored!");
     return false;
     }
  StopTransfer();
  StopReplay();
  if (fd_video >= 0 && fd_audio >= 0) {

     // Check DeviceName:

     if (!dvd) {
        esyslog(LOG_ERR, "ERROR: StartDVDplay: DVD device is (null)");
        return false;
        }

     // Create replay buffer:

     replayBuffer = new cDVDplayBuffer(this, fd_video, fd_audio, dvd, TitleID);
     if (replayBuffer)
        return true;
     else
        esyslog(LOG_ERR, "ERROR: can't allocate replaying buffer");
     }
  return false;
}
#endif //DVDSUPPORT

void cDvbApi::StopReplay(void)
{
  if (replayBuffer) {
     delete replayBuffer;
     replayBuffer = NULL;
     if (this == PrimaryDvbApi) {
        // let's explicitly switch the channel back in case it was in Transfer Mode:
        cChannel *Channel = Channels.GetByNumber(currentChannel);
        if (Channel)
           Channel->Switch(this, false);
        }
     }
}

void cDvbApi::Pause(void)
{
  if (replayBuffer)
     replayBuffer->Pause();
}

void cDvbApi::Play(void)
{
  if (replayBuffer)
     replayBuffer->Play();
}

void cDvbApi::Forward(void)
{
  if (replayBuffer)
     replayBuffer->Forward();
}

void cDvbApi::Backward(void)
{
  if (replayBuffer)
     replayBuffer->Backward();
}

void cDvbApi::SkipSeconds(int Seconds)
{
  if (replayBuffer)
     replayBuffer->SkipSeconds(Seconds);
}

int cDvbApi::SkipFrames(int Frames)
{
  if (replayBuffer)
     return replayBuffer->SkipFrames(Frames);
  return -1;
}

bool cDvbApi::GetIndex(int &Current, int &Total, bool SnapToIFrame)
{
  if (replayBuffer) {
     replayBuffer->GetIndex(Current, Total, SnapToIFrame);
     return true;
     }
  return false;
}

void cDvbApi::Goto(int Position, bool Still)
{
  if (replayBuffer)
     replayBuffer->Goto(Position, Still);
}

bool cDvbApi::CanToggleAudioTrack(void)
{
  return replayBuffer ? replayBuffer->CanToggleAudioTrack() : (aPid1 && aPid2 && aPid1 != aPid2);
}

bool cDvbApi::ToggleAudioTrack(void)
{
  if (replayBuffer) {
     replayBuffer->ToggleAudioTrack();
     return true;
     }
  else {
     int a = aPid2;
     aPid2 = aPid1;
     aPid1 = a;
     if (transferringFromDvbApi)
        return transferringFromDvbApi->ToggleAudioTrack();
     else {
        if (transferBuffer)
           transferBuffer->SetAudioPid(aPid1);
        return SetPids(transferBuffer != NULL);
        }
     }
  return false;
}

void cDvbApi::SetAudioCommand(const char *Command)
{
  delete audioCommand;
  audioCommand = strdup(Command);
}

// --- cEITScanner -----------------------------------------------------------

cEITScanner::cEITScanner(void)
{
  lastScan = lastActivity = time(NULL);
  currentChannel = 0;
  lastChannel = 0;
  numTransponders = 0;
  transponders = NULL;
}

cEITScanner::~cEITScanner()
{
  delete transponders;
}

bool cEITScanner::TransponderScanned(cChannel *Channel)
{
  for (int i = 0; i < numTransponders; i++) {
      if (transponders[i] == Channel->frequency)
         return true;
      }
  transponders = (int *)realloc(transponders, ++numTransponders * sizeof(int));
  transponders[numTransponders - 1] = Channel->frequency;
  return false;
}

void cEITScanner::Activity(void)
{
  if (currentChannel) {
     Channels.SwitchTo(currentChannel);
     currentChannel = 0;
     }
  lastActivity = time(NULL);
}

void cEITScanner::Process(void)
{
  if (Setup.EPGScanTimeout && Channels.MaxNumber() > 1) {
     time_t now = time(NULL);
     if (now - lastScan > ScanTimeout && now - lastActivity > ActivityTimeout) {
        for (int i = 0; i < cDvbApi::NumDvbApis; i++) {
            cDvbApi *DvbApi = cDvbApi::GetDvbApi(i + 1, MAXPRIORITY);
            if (DvbApi) {
               if (DvbApi != cDvbApi::PrimaryDvbApi || (cDvbApi::NumDvbApis == 1 && Setup.EPGScanTimeout && now - lastActivity > Setup.EPGScanTimeout * 3600)) {
                  if (!(DvbApi->Recording() || DvbApi->Replaying() || DvbApi->Transferring())) {
                     int oldCh = lastChannel;
                     int ch = oldCh + 1;
                     while (ch != oldCh) {
                           if (ch > Channels.MaxNumber()) {
                              ch = 1;
                              numTransponders = 0;
                              }
                           cChannel *Channel = Channels.GetByNumber(ch);
                           if (Channel && Channel->pnr && !TransponderScanned(Channel)) {
                              if (DvbApi == cDvbApi::PrimaryDvbApi && !currentChannel)
                                 currentChannel = DvbApi->Channel();
                              Channel->Switch(DvbApi, false);
                              lastChannel = ch;
                              break;
                              }
                           ch++;
                           }
                     }
                  }
               }
            }
        lastScan = time(NULL);
        }
     }
}

