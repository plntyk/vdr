/*
 * svdrp.h: Simple Video Disk Recorder Protocol
 *
 * See the main source file 'vdr.c' for copyright information and
 * how to reach the author.
 *
 * $Id: svdrp.h 1.5 2000/09/17 10:22:49 kls Exp $
 */

#ifndef __SVDRP_H
#define __SVDRP_H

#include "tools.h"

class cSocket {
private:
  int port;
  int sock;
  int queue;
  void Close(void);
public:
  cSocket(int Port, int Queue = 1);
  ~cSocket();
  bool Open(void);
  int Accept(void);
  };

class cSVDRP {
private:
  cSocket socket;
  cFile file;
  void Close(void);
  bool Send(const char *s, int length = -1);
  void Reply(int Code, const char *fmt, ...);
  void CmdCHAN(const char *Option);
  void CmdDELC(const char *Option);
  void CmdDELT(const char *Option);
  void CmdGRAB(const char *Option);
  void CmdHELP(const char *Option);
  void CmdHITK(const char *Option);
  void CmdLSTC(const char *Option);
  void CmdLSTT(const char *Option);
  void CmdMODC(const char *Option);
  void CmdMODT(const char *Option);
  void CmdMOVC(const char *Option);
  void CmdMOVT(const char *Option);
  void CmdNEWC(const char *Option);
  void CmdNEWT(const char *Option);
  void CmdUPDT(const char *Option);
  void Execute(char *Cmd);
public:
  cSVDRP(int Port);
  ~cSVDRP();
  void Process(void);
  };

#endif //__SVDRP_H
