// util/kaldi-io.cc

// Copyright 2009-2011  Microsoft Corporation;  Jan Silovsky

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//  http://www.apache.org/licenses/LICENSE-2.0

// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.
#include "util/kaldi-io.h"
#include "base/kaldi-math.h"
#include "util/text-utils.h"
#include "util/parse-options.h"
#include <cstdlib>
#include <errno.h>

#include "util/kaldi-pipebuf.h"
namespace kaldi {

#ifndef _MSC_VER // on VS, we don't need this type.
// could replace basic_pipebuf<char> with stdio_filebuf<char>, on some platforms.
// Would mean we could use less of our own code.
typedef basic_pipebuf<char> PipebufType;
#endif
}

namespace kaldi {

std::string PrintableRxfilename(std::string rxfilename) {
  if (rxfilename == "" || rxfilename == "-") return "standard input";
  else {
    // If this call to Escape later causes compilation issues,
    // just replace it with "return rxfilename"; it's only a
    // pretty-printing issue.
    return ParseOptions::Escape(rxfilename);
  }
}


std::string PrintableWxfilename(std::string wxfilename) {
  if (wxfilename == "" || wxfilename == "-") return "standard output";
  else {
    // If this call to Escape later causes compilation issues,
    // just replace it with "return rxfilename"; it's only a
    // pretty-printing issue.
    return ParseOptions::Escape(wxfilename);
  }
}


OutputType ClassifyWxfilename(const std::string &filename) {
  const char *c = filename.c_str();
  if (*c == '\0' || (*c == '-' && c[1] == '\0')) return kStandardOutput;  // "" or "-".
  else if (*c == '|') return kPipeOutput;  // An output pipe like "|blah".
  else if (isspace(*c) || isspace(c[filename.length()-1])) return kNoOutput;  // Leading or
  // trailing space: can't interpret this.
  else if ( (*c == 't'||*c == 'b') && c[1] == ',') {
    // We have detected that the user has supplied a wspecifier
    // or rspecifier (as in kaldi-table.h) where a wxfilename was
    // needed.  Since this is almost certain not to be a real filename
    // (and would cause confusion if it were a real filename), we
    // refuse to deal with it.
    return kNoOutput;
  } else {
    const char *d = c;
    while (d[1] != '\0') d++;  // go to last char.
    if (*d == '|' || isspace(*d)) return kNoOutput;  // An input pipe (not allowed in
    // this context) or trailing space which is just wrong.
    else if (isdigit(*d)) {
      // OK, it could be a file, but we have to see if it's an offset into a file,
      // which is not allowed.
      while (isdigit(*d) && d > c) d--;
      if (*d == ':') return kNoOutput;  // Filename is like some_file:12345; not allowed,
      else return kFileOutput;
      // as we cannot write to an offset into a file (and if we interpreted it as an
      // actual filename, the reading code would misinterpret it as an offset.
    } else {
      // at this point it matched no other pattern so we assume a filename, but we
      // check for '|' as it's a common source of errors to have pipe commands without
      // the pipe in the right place.  Say that it can't be classified.
      if (strchr(c, '|') != NULL) {
        KALDI_WARN << "Trying to classify wxfilename with pipe symbol in the wrong place (pipe without | at the beginning?): " << filename;
        return kNoOutput;
      }
      return kFileOutput;  // matched no other pattern: assume it's an actual filename.
    }
  }
}


InputType ClassifyRxfilename(const std::string &filename) {
  const char *c = filename.c_str();
  if (*c == '\0' || (*c == '-' && c[1] == '\0')) return kStandardInput;  // "" or "-".
  else if (*c == '|') return kNoInput;  // An output pipe like "|blah": not valid for input.
  else if (isspace(*c) || isspace(c[filename.length()-1])) return kNoInput;  // Leading or
  // trailing space.
  else if ( (*c == 't'||*c == 'b') && c[1] == ',') {
    // We have detected that the user has supplied a wspecifier
    // or rspecifier (as in kaldi-table.h) where a wxfilename was
    // needed.  Since this is almost certain not to be a real filename
    // (and would cause a lot of confusion if it were a real filename), we
    // refuse to deal with it upfront.
    return kNoInput;
  } else {
    const char *d = c;
    while (d[1] != '\0') d++;  // go to last char.
    if (*d == '|') return kPipeInput;  // an input pipe.
    if (isspace(*d)) return kNoInput;  // trailing space which is never valid.
    else if (isdigit(*d)) {
      // OK, it could be an offset into a file
      // which is not allowed.
      while (isdigit(*d) && d > c) d--;
      if (*d == ':') return kOffsetFileInput;  // Filename is like some_file:12345
      else return kFileInput;
    } else {
      // at this point it matched no other pattern so we assume a filename, but we
      // check for '|' as it's a common source of errors to have pipe commands without
      // the pipe in the right place.  Say that it can't be classified in this case.
      if (strchr(c, '|') != NULL) {
        KALDI_WARN << "Trying to classify rxfilename with pipe symbol in the wrong place (pipe without | at the end?): " << filename;
        return kNoInput;
      }
      return kFileInput;  // matched no other pattern: assume it's an actual filename.
    }
  }
}

#ifdef _MSC_VER
  // A popen implementation that passes the command line through cygwin
  // bash.exe. This is necessary since some piped commands are cygwin links
  // (e. g. fgrep is a soft link to grep), and some are #!-files, such as
  // gunzip which is a shell script that invokes gzip, or kaldi's own run.pl
  // which is a perl script.
  //
  // _popen uses cmd.exe or whatever shell is specified via the COMSPEC
  // variable. Unfortunately, it adds a hardcoded " /c " to it, so we cannot
  // just substitute the environment variable COMSPEC to point to bash.exe.
  // Instead, quote the command and pass it to bash via its -c switch.
  FILE *popen(const char* command, const char* mode) {
    // To speed up command launch marginally, optionally accept full path
    // to bash.exe. This will not work if the path contains spaces, but
    // no sane person would install cygwin into a space-ridden path.
    const char* bash_exe = std::getenv("BASH_EXE");
    std::string qcmd(bash_exe != nullptr ? bash_exe : "bash.exe");
    qcmd += " -c \"";
    for (; *command; ++command) {
      if (*command == '\"' || *command == '\\')
        qcmd += '\\';
      qcmd += *command;
    }
    qcmd += '\"';

    return _popen(qcmd.c_str(), mode);
  }
#endif  // _MSC_VER

#ifdef _MSC_VER
// Massage filename into an OS native format. This is chiefly for Windows.
// Since shell scripts run under cygwin, they use cygwin's own mount table
// and a mapping to the file system. It is quite possible to create quite an
// intricate mapping that only own cygwin API would be able to untangle.
// Unfortunately, the API to map between filenames is not available to non-
// cygwin programs. Running cygpath for every file operation would as well
// be cumbersome. So this is only a simplistic path resolution, assuming that
// the default cygwin prefix /cygdrive is used, and that all resolved
// unix-style full paths end up prefixed with /cygdrive. This is quite a
// sensible approach. We'll also try to map /dev/null and /tmp/**, die on all
// other /dev/** and warn about all other rooted paths.
static bool prefixp(const std::string& pfx, const std::string& str) {
  return pfx.length() <= str.length() &&
    std::equal(pfx.begin(), pfx.end(), str.begin());
}

static std::string cygprefix("/cygdrive/");  // Ease the use of a -D later.

static std::string map_os_path_no_tmp(const std::string &filename) {
  // UNC(?), relative, native Windows and empty paths are ok already.
  if (prefixp("//", filename) || !prefixp("/", filename))
    return filename;

  // /dev/...
  if (filename == "/dev/null")
    return "\\\\.\\nul";
  if (prefixp("/dev/", filename)) {
      KALDI_ERR << "Unable to resolve path '" << filename
                << "' - only have /dev/null here.";
      return "\\\\.\\invalid";
  }

  // /cygdrive/?[/....]
  // 0----5---90 1
  int preflen = cygprefix.size();
  if (prefixp(cygprefix, filename)
      && filename.size() >= preflen + 1 && isalpha(filename[preflen])
      && (filename.size() == preflen + 1 || filename[preflen + 1] == '/')) {
    return std::string() + filename[preflen] + ':' +
       (filename.size() > preflen + 1 ? filename.substr(preflen + 1) : "/");
  }

  KALDI_WARN << "Unable to resolve path '" << filename
             << "' - cannot map unix prefix. "
             << "Will go on, but breakage will likely ensue.";
  return filename;
}

// extern for unit testing.
std::string map_os_path(const std::string &filename) {
  // /tmp[/....]
  if (filename != "/tmp" && !prefixp("/tmp/", filename)) {
    return map_os_path_no_tmp(filename);
  }
  char *tmpdir = std::getenv("TMP");
  if (tmpdir == nullptr)
    tmpdir = std::getenv("TEMP");
  if (tmpdir == nullptr) {
    KALDI_ERR << "Unable to resolve path '" << filename
              << "' - unable to find temporary directory. Set TMP.";
    return filename;
  }
  // Do not return the result: cygwin environment actually may have unix-style paths.
  return map_os_path_no_tmp(std::string(tmpdir) + filename.substr(4));
}

#else  // _MSC_VER
// Identity on unix-ey file systems.
#define map_os_path(x) x
#endif  // _MSC_VER

class OutputImplBase {
 public:
  // Open will open it as a file (no header), and return true
  // on success.  It cannot be called on an already open stream.
  virtual bool Open(const std::string &filename, bool binary) = 0;
  virtual std::ostream &Stream() = 0;
  virtual bool Close() = 0;
  virtual ~OutputImplBase() { }
};


class FileOutputImpl: public OutputImplBase {
 public:
  virtual bool Open(const std::string &filename, bool binary) {
    if (os_.is_open()) KALDI_ERR << "FileOutputImpl::Open(), "
                                << "open called on already open file.";
    filename_ = filename;
    os_.open(map_os_path(filename_).c_str(),
             binary ? std::ios_base::out | std::ios_base::binary
                    : std::ios_base::out);
    return os_.is_open();
  }

  virtual std::ostream &Stream() {
    if (!os_.is_open())
      KALDI_ERR << "FileOutputImpl::Stream(), file is not open.";
      // I believe this error can only arise from coding error.
    return os_;
  }

  virtual bool Close() {
    if (!os_.is_open()) KALDI_ERR << "FileOutputImpl::Close(), file is not open.";
    // I believe this error can only arise from coding error.
    os_.close();
    return !(os_.fail());
  }
  virtual ~FileOutputImpl() {
    if (os_.is_open()) {
      os_.close();
      if (os_.fail())
        KALDI_ERR << "Error closing output file " << filename_;
    }
  }
 private:
  std::string filename_;
  std::ofstream os_;
};

class StandardOutputImpl: public OutputImplBase {
 public:
  StandardOutputImpl(): is_open_(false) { }

  virtual bool Open(const std::string &filename, bool binary) {
    if (is_open_) KALDI_ERR << "StandardOutputImpl::Open(), "
                     "open called on already open file.";
#ifdef _MSC_VER
    _setmode(_fileno(stdout), binary ? _O_BINARY : _O_TEXT);
#endif
    is_open_ = std::cout.good();
    return is_open_;
  }

  virtual std::ostream &Stream() {
    if (!is_open_) KALDI_ERR << "StandardOutputImpl::Stream(), object not initialized.";
    // I believe this error can only arise from coding error.
    return std::cout;
  }

  virtual bool Close() {
    if (!is_open_) KALDI_ERR << "StandardOutputImpl::Close(), file is not open.";
    is_open_ = false;
    std::cout << std::flush;
    return !(std::cout.fail());
  }
  virtual ~StandardOutputImpl() {
    if (is_open_) {
      std::cout << std::flush;
      if (std::cout.fail())
        KALDI_ERR << "Error writing to standard output";
    }
  }
 private:
  bool is_open_;
};

class PipeOutputImpl: public OutputImplBase {
 public:
  PipeOutputImpl(): f_ (NULL), os_(NULL) { }

  virtual bool Open(const std::string &wxfilename, bool binary) {
    filename_ = wxfilename;
    KALDI_ASSERT(f_ == NULL);  // Make sure closed.
    KALDI_ASSERT(wxfilename.length() != 0 && wxfilename[0] == '|');  // should start with '|'
    std::string cmd_name(wxfilename, 1);
#ifdef _MSC_VER
    f_ = popen(cmd_name.c_str(), (binary ? "wb" : "w"));
#else
    f_ = popen(cmd_name.c_str(), "w");
#endif
    if (!f_) {  // Failure.
      KALDI_WARN << "Failed opening pipe for writing, command is: "
                 << cmd_name << ", errno is " << strerror(errno);
      return false;
    } else {
#ifndef _MSC_VER
      fb_ = new PipebufType(f_,  // Using this constructor won't make the destructor
                                  // try to close the stream when we're done.
                                  (binary ? std::ios_base::out|std::ios_base::binary
                                   :std::ios_base::out));
      KALDI_ASSERT(fb_ != NULL);  // or would be alloc error.
      os_ = new std::ostream(fb_);
#else
      os_ = new std::ofstream(f_);
#endif
      return os_->good();
    }
  }

  virtual std::ostream &Stream() {
    if (os_ == NULL) KALDI_ERR << "PipeOutputImpl::Stream(), object not initialized.";
    // I believe this error can only arise from coding error.
    return *os_;
  }

  virtual bool Close() {
    if (os_ == NULL) KALDI_ERR << "PipeOutputImpl::Close(), file is not open.";
    bool ok = true;
    os_->flush();
    if (os_->fail()) ok = false;
    delete os_;
    os_ = NULL;
    int status;
#ifdef _MSC_VER
    status = _pclose(f_);
#else
    status = pclose(f_);
#endif
    if (status)
      KALDI_WARN << "Pipe " << filename_ << " had nonzero return status "
                 << status;
    f_ = NULL;
#ifndef _MSC_VER
    delete fb_;
    fb_ = NULL;
#endif
    return ok;
  }
  virtual ~PipeOutputImpl() {
    if (os_) {
      if (! Close())
        KALDI_ERR << "Error writing to pipe " << PrintableWxfilename(filename_);
    }
  }
 private:
  std::string filename_;
  FILE *f_;
#ifndef _MSC_VER
  PipebufType *fb_;
#endif
  std::ostream *os_;
};



class InputImplBase {
 public:
  // Open will open it as a file, and return true on success.
  // May be called twice only for kOffsetFileInput (otherwise,
  // if called twice, we just create a new Input object, to avoid
  // having to deal with the extra hassle of reopening with the
  // same object.
  // Note that we will to call Open with true (binary) for
  // for text-mode Kaldi files; the only actual text-mode input
  // is for non-Kaldi files.
  virtual bool Open(const std::string &filename, bool binary) = 0;
  virtual std::istream &Stream() = 0;
  virtual void Close() = 0;  // don't bother checking failure
  // on close for input streams.
  virtual InputType MyType() = 0;  // Because if it's kOffsetFileInput, we may call Open twice
  // (has efficiency benefits).

  virtual ~InputImplBase() { }
};

class FileInputImpl: public InputImplBase {
 public:
  virtual bool Open(const std::string &filename, bool binary) {
    if (is_.is_open()) KALDI_ERR << "FileInputImpl::Open(), "
                                << "open called on already open file.";
    is_.open(map_os_path(filename).c_str(),
             binary ? std::ios_base::in | std::ios_base::binary
                    : std::ios_base::in);
    return is_.is_open();
  }

  virtual std::istream &Stream() {
    if (!is_.is_open()) KALDI_ERR << "FileInputImpl::Stream(), file is not open.";
    // I believe this error can only arise from coding error.
    return is_;
  }

  virtual void Close() {
    if (!is_.is_open()) KALDI_ERR << "FileInputImpl::Close(), file is not open.";
    // I believe this error can only arise from coding error.
    is_.close();
    // Don't check status.
  }

  virtual InputType MyType() { return kFileInput; }

  virtual ~FileInputImpl() {
    // Stream will automatically be closed, and we don't care about
    // whether it fails.
  }
 private:
  std::ifstream is_;
};


class StandardInputImpl: public InputImplBase {
 public:
  StandardInputImpl(): is_open_(false) { }

  virtual bool Open(const std::string &filename, bool binary) {
    if (is_open_) KALDI_ERR << "StandardInputImpl::Open(), "
                     "open called on already open file.";
    is_open_ = true;
#ifdef _MSC_VER
    _setmode(_fileno(stdin), binary ? _O_BINARY : _O_TEXT);
#endif
    return true;  // Don't check good() because would be false if
    // eof, which may be valid input.
  }

  virtual std::istream &Stream() {
    if (!is_open_) KALDI_ERR << "StandardInputImpl::Stream(), object not initialized.";
    // I believe this error can only arise from coding error.
    return std::cin;
  }

  virtual InputType MyType() { return kStandardInput; }

  virtual void Close() {
    if (!is_open_) KALDI_ERR << "StandardInputImpl::Close(), file is not open.";
    is_open_ = false;
  }
  virtual ~StandardInputImpl() { }
 private:
  bool is_open_;
};

class PipeInputImpl: public InputImplBase {
 public:
  PipeInputImpl(): f_ (NULL), is_(NULL) { }

  virtual bool Open(const std::string &rxfilename, bool binary) {
    filename_ = rxfilename;
    KALDI_ASSERT(f_ == NULL);  // Make sure closed.
    KALDI_ASSERT(rxfilename.length() != 0 &&
           rxfilename[rxfilename.length()-1] == '|');  // should end with '|'
    std::string cmd_name(rxfilename, 0, rxfilename.length()-1);
#ifdef _MSC_VER
    f_ = popen(cmd_name.c_str(), (binary ? "rb" : "r"));
#else
    f_ = popen(cmd_name.c_str(), "r");
#endif

    if (!f_) {  // Failure.
      KALDI_WARN << "Failed opening pipe for reading, command is: "
                 << cmd_name << ", errno is " << strerror(errno);
      return false;
    } else {
#ifndef _MSC_VER
      fb_ = new PipebufType(f_,  // Using this constructor won't lead the
                                 // destructor to close the stream.
                                 (binary ? std::ios_base::in|std::ios_base::binary
                                  :std::ios_base::in));
      KALDI_ASSERT(fb_ != NULL);  // or would be alloc error.
      is_ = new std::istream(fb_);
#else
      is_ = new std::ifstream(f_);
#endif
      if (is_->fail() || is_->bad()) return false;
      if (is_->eof()) {
        KALDI_WARN << "Pipe opened with command "
                   << PrintableRxfilename(rxfilename)
                   << " is empty.";
        // don't return false: empty may be valid.
      }
      return true;
    }
  }

  virtual std::istream &Stream() {
    if (is_ == NULL) KALDI_ERR << "PipeInputImpl::Stream(), object not initialized.";
    // I believe this error can only arise from coding error.
    return *is_;
  }

  virtual void Close() {
    if (is_ == NULL) KALDI_ERR << "PipeInputImpl::Close(), file is not open.";
    delete is_;
    is_ = NULL;
    int status;
#ifdef _MSC_VER
    status = _pclose(f_);
#else
    status = pclose(f_);
#endif
    if (status)
      KALDI_WARN << "Pipe " << filename_ << " had nonzero return status "
                 << status;
    f_ = NULL;
#ifndef _MSC_VER
    delete fb_;
    fb_ = NULL;
#endif
  }
  virtual ~PipeInputImpl() {
    if (is_)
      Close();
  }
  virtual InputType MyType() { return kPipeInput; }
 private:
  std::string filename_;
  FILE *f_;
#ifndef _MSC_VER
  PipebufType *fb_;
#endif
  std::istream *is_;
};

/*
#else

// Just have an empty implementation of the pipe input that crashes if
// called.
class PipeInputImpl: public InputImplBase {
 public:
  PipeInputImpl() { KALDI_ASSERT(0 && "Pipe input not yet supported on this platform."); }
  virtual bool Open(const std::string, bool) { return 0; }
  virtual std::istream &Stream() const { return NULL; }
  virtual void Close() {}
  virtual InputType MyType() { return kPipeInput; }
};

#endif
*/

class OffsetFileInputImpl: public InputImplBase {
  // This class is a bit more complicated than the

 public:
  // splits a filename like /my/file:123 into /my/file and the
  // number 123.  Crashes if not this format.
  static void SplitFilename(const std::string &rxfilename,
                            std::string *filename,
                            size_t *offset) {
    size_t pos = rxfilename.find_last_of(':');
    KALDI_ASSERT(pos != std::string::npos);  // would indicate error in calling code,
    // as the filename is supposed to be of the correct form at this
    // point.
    *filename = std::string(rxfilename, 0, pos);
    std::string number(rxfilename, pos+1);
    bool ans = ConvertStringToInteger(number, offset);
    if (!ans)
      KALDI_ERR << "Cannot get offset from filename " << rxfilename
                << " (possibly you compiled in 32-bit and have a >32-bit"
                << " byte offset into a file; you'll have to compile 64-bit.";
  }

  bool Seek(size_t offset) {
    size_t cur_pos = is_.tellg();
    if (cur_pos == offset) return true;
    else if (cur_pos<offset && cur_pos+100 > offset) {
      // We're close enough that it may be faster to just
      // read that data, rather than seek.
      for (size_t i = cur_pos; i < offset; i++)
        is_.get();
      return (is_.tellg() == std::streampos(offset));
    }
    // Try to actually seek.
    is_.seekg(offset, std::ios_base::beg);
    if (is_.fail()) {  // failbit or badbit is set [error happened]
      is_.close();
      return false;  // failure.
    } else {
      is_.clear();  // Clear any failure bits (e.g. eof).
      return true;  // success.
    }
  }

  // This Open routine is unusual in that it is designed to work even
  // if it was already open.  This for efficiency when seeking multiple
  // times.
  virtual bool Open(const std::string &rxfilename, bool binary) {
    if (is_.is_open()) {
      // We are opening when we have an already-open file.
      // We may have to seek within this file, or else close it and
      // open a different one.
      std::string tmp_filename;
      size_t offset;
      SplitFilename(rxfilename, &tmp_filename, &offset);
      if (tmp_filename == filename_ && binary == binary_) {  // Just seek
        is_.clear();  // clear fail bit, etc.
        return Seek(offset);
      } else {
        is_.close();  // don't bother checking error status of is_.
        filename_ = tmp_filename;
        is_.open(map_os_path(filename_).c_str(),
                 binary ? std::ios_base::in | std::ios_base::binary
                        : std::ios_base::in);
        if (!is_.is_open()) return false;
        else return Seek(offset);
      }
    } else {
      size_t offset;
      SplitFilename(rxfilename, &filename_, &offset);
      binary_ = binary;
      is_.open(map_os_path(filename_).c_str(),
                binary ? std::ios_base::in | std::ios_base::binary
                      : std::ios_base::in);
      if (!is_.is_open()) return false;
      else return Seek(offset);
    }
  }

  virtual std::istream &Stream() {
    if (!is_.is_open()) KALDI_ERR << "FileInputImpl::Stream(), file is not open.";
    // I believe this error can only arise from coding error.
    return is_;
  }

  virtual void Close() {
    if (!is_.is_open()) KALDI_ERR << "FileInputImpl::Close(), file is not open.";
    // I believe this error can only arise from coding error.
    is_.close();
    // Don't check status.
  }

  virtual InputType MyType() { return kOffsetFileInput; }

  virtual ~OffsetFileInputImpl() {
    // Stream will automatically be closed, and we don't care about
    // whether it fails.
  }
 private:
  std::string filename_;  // the actual filename
  bool binary_;  // true if was opened in binary mode.
  std::ifstream is_;
};


Output::Output(const std::string &rxfilename, bool binary, bool write_header): impl_(NULL) {
  if (!Open(rxfilename, binary, write_header))  {
    if (impl_) {
      delete impl_;
      impl_ = NULL;
    }
    KALDI_ERR << "Error opening output stream " <<
        PrintableWxfilename(rxfilename);
  }
}

bool Output::Close() {
  if (!impl_) return false;  // error to call Close if not open.
  else {
    bool ans = impl_->Close();
    delete impl_;
    impl_ = NULL;
    return ans;
  }
}

Output::~Output() {
  if (impl_) {
    bool ok = impl_->Close();
    delete impl_;
    impl_ = NULL;
    if (!ok)
      KALDI_ERR << "Error closing output file " <<
          PrintableWxfilename(filename_);
  }
}

std::ostream &Output::Stream() {  // will throw if not open; else returns stream.
  if (!impl_) KALDI_ERR << "Output::Stream() called but not open.";
  return impl_->Stream();
}

bool Output::Open(const std::string &wxfn, bool binary, bool header) {
  // Consolidate all the types of Open calls here, since they're basically doing the
  // same thing.

  if (IsOpen()) {
    if (!Close()) {  // Throw here rather than return status, as it's an error about
      // something else: if the user wanted to avoid the exception he/she could have
      // called Close().
      KALDI_ERR << "Output::Open(), failed to close output stream: "
                << PrintableWxfilename(filename_);
    }
  }

  filename_ = wxfn;

  OutputType type = ClassifyWxfilename(wxfn);
  KALDI_ASSERT(impl_ == NULL);

  if (type ==  kFileOutput) {
    impl_ = new FileOutputImpl();
  } else if (type == kStandardOutput) {
    impl_ = new StandardOutputImpl();
  } else if (type == kPipeOutput) {
    impl_ = new PipeOutputImpl();
  } else {  // type == kNoOutput
    KALDI_WARN << "Invalid output filename format "<<
        PrintableWxfilename(wxfn);
    return false;
  }
  if (!impl_->Open(wxfn, binary)) {
    delete impl_;
    impl_ = NULL;
    return false;  // failed to open.
  } else {  // successfully opened it.
    if (header) {
      InitKaldiOutputStream(impl_->Stream(), binary);
      bool ok = impl_->Stream().good();  // still OK?
      if (!ok) {
        delete impl_;
        impl_ = NULL;
        return false;
      }
      return true;
    } else return true;
  }
}


Input::Input(const std::string &rxfilename, bool *binary): impl_(NULL) {
  if (!Open(rxfilename, binary)) {
    KALDI_ERR << "Error opening input stream "
              << PrintableRxfilename(rxfilename);
  }
}

void Input::Close() {
  if (impl_) {
    delete impl_;
    impl_ = NULL;
  }
}

bool Input::OpenInternal(const std::string &rxfilename,
                         bool file_binary,
                         bool *contents_binary) {
  InputType type = ClassifyRxfilename(rxfilename);
  if (IsOpen()) {
    // May have to close the stream first.
    if (type == kOffsetFileInput && impl_->MyType() == kOffsetFileInput) {
      // We want to use the same object to Open... this is in case
      // the files are the same, so we can just seek.
      if (!impl_->Open(rxfilename, file_binary)) {  // true is binary mode-- always open in binary.
        delete impl_;
        impl_ = NULL;
        return false;
      }
      // read the binary header, if requested.
      if (contents_binary != NULL)
        return InitKaldiInputStream(impl_->Stream(), contents_binary);
      else return true;
    } else {
      Close();
      // and fall through to code below which actually opens the file.
    }
  }
  if (type ==  kFileInput) {
    impl_ = new FileInputImpl();
  } else if (type == kStandardInput) {
    impl_ = new StandardInputImpl();
  } else if (type == kPipeInput) {
    impl_ = new PipeInputImpl();
  } else if (type == kOffsetFileInput) {
    impl_ = new OffsetFileInputImpl();
  } else {  // type == kNoInput
    KALDI_WARN << "Invalid input filename format "<<
        PrintableRxfilename(rxfilename);
    return false;
  }
  if (!impl_->Open(rxfilename, file_binary)) {  // true is binary mode-- always read in binary.
    delete impl_;
    impl_ = NULL;
    return false;
  }
  if (contents_binary != NULL)
    return InitKaldiInputStream(impl_->Stream(), contents_binary);
  else return true;
}


Input::~Input() { if (impl_) Close(); }


std::istream &Input::Stream() {
  if (!IsOpen()) KALDI_ERR << "Input::Stream(), not open.";
  return impl_->Stream();
}



}  // end namespace kaldi
