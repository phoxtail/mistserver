/// \file stream.cpp
/// Utilities for handling streams.

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <semaphore.h>
#include "json.h"
#include "stream.h"
#include "procs.h"
#include "config.h"
#include "socket.h"
#include "defines.h"
#include "shared_memory.h"
#include "dtsc.h"

std::string Util::getTmpFolder() {
  std::string dir;
  char * tmp_char = 0;
  if (!tmp_char) {
    tmp_char = getenv("TMP");
  }
  if (!tmp_char) {
    tmp_char = getenv("TEMP");
  }
  if (!tmp_char) {
    tmp_char = getenv("TMPDIR");
  }
  if (tmp_char) {
    dir = tmp_char;
    dir += "/mist";
  } else {
#if defined(_WIN32) || defined(_CYGWIN_)
    dir = "C:/tmp/mist";
#else
    dir = "/tmp/mist";
#endif
  }
  if (access(dir.c_str(), 0) != 0) {
    mkdir(dir.c_str(), S_IRWXU | S_IRWXG | S_IRWXO); //attempt to create mist folder - ignore failures
  }
  return dir + "/";
}

/// Filters the streamname, removing invalid characters and converting all
/// letters to lowercase. If a '?' character is found, everything following
/// that character is deleted. The original string is modified. If a '+' or space
/// exists, then only the part before that is sanitized.
void Util::sanitizeName(std::string & streamname) {
  //strip anything that isn't numbers, digits or underscores
  size_t index = streamname.find_first_of("+ ");
  if(index != std::string::npos){
    std::string preplus = streamname.substr(0,index);
    sanitizeName(preplus);
    std::string postplus = streamname.substr(index+1);
    if (postplus.find('?') != std::string::npos){
      postplus = postplus.substr(0, (postplus.find('?')));
    }
    streamname = preplus+"+"+postplus;
    return;
  }
  for (std::string::iterator i = streamname.end() - 1; i >= streamname.begin(); --i) {
    if (*i == '?') {
      streamname.erase(i, streamname.end());
      break;
    }
    if ( !isalpha( *i) && !isdigit( *i) && *i != '_' && *i != '.'){
      streamname.erase(i);
    } else {
      *i = tolower(*i);
    }
  }
}

JSON::Value Util::getStreamConfig(std::string streamname){
  JSON::Value result;
  if (streamname.size() > 100){
    FAIL_MSG("Stream opening denied: %s is longer than 100 characters (%lu).", streamname.c_str(), streamname.size());
    return result;
  }
  IPC::sharedPage mistConfOut(SHM_CONF, DEFAULT_CONF_PAGE_SIZE, false, false);
  IPC::semaphore configLock(SEM_CONF, O_CREAT | O_RDWR, ACCESSPERMS, 1);
  configLock.wait();
  DTSC::Scan config = DTSC::Scan(mistConfOut.mapped, mistConfOut.len);

  sanitizeName(streamname);
  std::string smp = streamname.substr(0, streamname.find_first_of("+ "));
  //check if smp (everything before + or space) exists
  DTSC::Scan stream_cfg = config.getMember("streams").getMember(smp);
  if (!stream_cfg){
    DEBUG_MSG(DLVL_MEDIUM, "Stream %s not configured", streamname.c_str());
  }else{
    result = stream_cfg.asJSON();
  }
  configLock.post();//unlock the config semaphore
  return result;
}

/// Checks if the given streamname has an active input serving it. Returns true if this is the case.
/// Assumes the streamname has already been through sanitizeName()!
bool Util::streamAlive(std::string & streamname){
  char semName[NAME_BUFFER_SIZE];
  snprintf(semName, NAME_BUFFER_SIZE, SEM_INPUT, streamname.c_str());
  IPC::semaphore playerLock(semName, O_RDWR, ACCESSPERMS, 1, true);
  if (!playerLock){return false;}
  if (!playerLock.tryWait()) {
    playerLock.close();
    return true;
  }else{
    playerLock.post();
    playerLock.close();
    return false;
  }
}

/// Assures the input for the given stream name is active.
/// Does stream name sanitation first, followed by a stream name length check (<= 100 chars).
/// Then, checks if an input is already active by running streamAlive(). If yes, return true.
/// If no, loads up the server configuration and attempts to start the given stream according to current configuration.
/// At this point, fails and aborts if MistController isn't running.
bool Util::startInput(std::string streamname, std::string filename, bool forkFirst, bool isProvider) {
  sanitizeName(streamname);
  if (streamname.size() > 100){
    FAIL_MSG("Stream opening denied: %s is longer than 100 characters (%lu).", streamname.c_str(), streamname.size());
    return false;
  }
  //Check if the stream is already active.
  //If yes, don't activate again to prevent duplicate inputs.
  //It's still possible a duplicate starts anyway, this is caught in the inputs initializer.
  //Note: this uses the _whole_ stream name, including + (if any).
  //This means "test+a" and "test+b" have separate locks and do not interact with each other.
  if (streamAlive(streamname)){
    DEBUG_MSG(DLVL_MEDIUM, "Stream %s already active; continuing", streamname.c_str());
    return true;
  }

  //Attempt to load up configuration and find this stream
  IPC::sharedPage mistConfOut(SHM_CONF, DEFAULT_CONF_PAGE_SIZE);
  IPC::semaphore configLock(SEM_CONF, O_CREAT | O_RDWR, ACCESSPERMS, 1);
  //Lock the config to prevent race conditions and corruption issues while reading
  configLock.wait();
  DTSC::Scan config = DTSC::Scan(mistConfOut.mapped, mistConfOut.len);
  //Abort if no config available
  if (!config){
    FAIL_MSG("Configuration not available, aborting! Is MistController running?");
    configLock.post();//unlock the config semaphore
    return false;
  }
  //Find stream base name
  std::string smp = streamname.substr(0, streamname.find_first_of("+ "));
  //check if base name (everything before + or space) exists
  DTSC::Scan stream_cfg = config.getMember("streams").getMember(smp);
  if (!stream_cfg){
    DEBUG_MSG(DLVL_HIGH, "Stream %s not configured - attempting to ignore", streamname.c_str());
  }
  
  //Only use configured source if not manually overridden. Abort if no config is available.
  if (!filename.size()){
    if (!stream_cfg){
      DEBUG_MSG(DLVL_MEDIUM, "Stream %s not configured, no source manually given, cannot start", streamname.c_str());
      configLock.post();//unlock the config semaphore
      return false;
    }
    filename = stream_cfg.getMember("source").asString();
  }
  
  //check in curConf for capabilities-inputs-<naam>-priority/source_match
  std::string player_bin;
  bool selected = false;
  long long int curPrio = -1;
  DTSC::Scan inputs = config.getMember("capabilities").getMember("inputs");
  DTSC::Scan input;
  unsigned int input_size = inputs.getSize();
  bool noProviderNoPick = false;
  for (unsigned int i = 0; i < input_size; ++i){
    DTSC::Scan tmp_input = inputs.getIndice(i);
    
    //if match voor current stream && priority is hoger dan wat we al hebben
    if (tmp_input.getMember("source_match") && curPrio < tmp_input.getMember("priority").asInt()){
      if (tmp_input.getMember("source_match").getSize()){
        for(unsigned int j = 0; j < tmp_input.getMember("source_match").getSize(); ++j){
          std::string source = tmp_input.getMember("source_match").getIndice(j).asString();
          std::string front = source.substr(0,source.find('*'));
          std::string back = source.substr(source.find('*')+1);
          MEDIUM_MSG("Checking input %s: %s (%s)", inputs.getIndiceName(i).c_str(), tmp_input.getMember("name").asString().c_str(), source.c_str());
          
          if (filename.substr(0,front.size()) == front && filename.substr(filename.size()-back.size()) == back){
            if (tmp_input.getMember("non-provider") && !isProvider){
              noProviderNoPick = true;
              continue;
            }
            player_bin = Util::getMyPath() + "MistIn" + tmp_input.getMember("name").asString();
            curPrio = tmp_input.getMember("priority").asInt();
            selected = true;
            input = tmp_input;
          }
        }
      }else{
        std::string source = tmp_input.getMember("source_match").asString();
        std::string front = source.substr(0,source.find('*'));
        std::string back = source.substr(source.find('*')+1);
        MEDIUM_MSG("Checking input %s: %s (%s)", inputs.getIndiceName(i).c_str(), tmp_input.getMember("name").asString().c_str(), source.c_str());
        
        if (filename.substr(0,front.size()) == front && filename.substr(filename.size()-back.size()) == back){
          if (tmp_input.getMember("non-provider") && !isProvider){
            noProviderNoPick = true;
            continue;
          }
          player_bin = Util::getMyPath() + "MistIn" + tmp_input.getMember("name").asString();
          curPrio = tmp_input.getMember("priority").asInt();
          selected = true;
          input = tmp_input;
        }
      }

    }
  }
  
  if (!selected){
    configLock.post();//unlock the config semaphore
    if (noProviderNoPick){
      INFO_MSG("Not a media provider for stream %s: %s", streamname.c_str(), filename.c_str());
    }else{
      FAIL_MSG("No compatible input found for stream %s: %s", streamname.c_str(), filename.c_str());
    }
    return false;
  }

  //copy the necessary arguments to separate storage so we can unlock the config semaphore safely
  std::map<std::string, std::string> str_args;
  //check required parameters
  DTSC::Scan required = input.getMember("required");
  unsigned int req_size = required.getSize();
  for (unsigned int i = 0; i < req_size; ++i){
    std::string opt = required.getIndiceName(i);
    if (!stream_cfg.getMember(opt)){
      configLock.post();//unlock the config semaphore
      FAIL_MSG("Required parameter %s for stream %s missing", opt.c_str(), streamname.c_str());
      return false;
    }
    str_args[required.getIndice(i).getMember("option").asString()] = stream_cfg.getMember(opt).asString();
  }
  //check optional parameters
  DTSC::Scan optional = input.getMember("optional");
  unsigned int opt_size = optional.getSize();
  for (unsigned int i = 0; i < opt_size; ++i){
    std::string opt = optional.getIndiceName(i);
    VERYHIGH_MSG("Checking optional %u: %s", i, opt.c_str());
    if (stream_cfg.getMember(opt)){
      str_args[optional.getIndice(i).getMember("option").asString()] = stream_cfg.getMember(opt).asString();
    }
  }
  
  //finally, unlock the config semaphore
  configLock.post();

  INFO_MSG("Starting %s -s %s %s", player_bin.c_str(), streamname.c_str(), filename.c_str());
  char * argv[30] = {(char *)player_bin.c_str(), (char *)"-s", (char *)streamname.c_str(), (char *)filename.c_str()};
  int argNum = 3;
  std::string debugLvl;
  if (Util::Config::printDebugLevel != DEBUG && !str_args.count("--debug")){
    debugLvl = JSON::Value((long long)Util::Config::printDebugLevel).asString();
    argv[++argNum] = (char *)"--debug";
    argv[++argNum] = (char *)debugLvl.c_str();
  }
  for (std::map<std::string, std::string>::iterator it = str_args.begin(); it != str_args.end(); ++it){
    argv[++argNum] = (char *)it->first.c_str();
    argv[++argNum] = (char *)it->second.c_str();
    INFO_MSG("  Option %s = %s", it->first.c_str(), it->second.c_str());
  }
  argv[++argNum] = (char *)0;
  
  int pid = 0;
  if (forkFirst){
    DEBUG_MSG(DLVL_DONTEVEN, "Forking");
    pid = fork();
    if (pid == -1) {
      FAIL_MSG("Forking process for stream %s failed: %s", streamname.c_str(), strerror(errno));
      return false;
    }
  }else{
    DEBUG_MSG(DLVL_DONTEVEN, "Not forking");
  }
  
  if (pid == 0){
    Socket::Connection io(0, 1);
    io.close();
    DEBUG_MSG(DLVL_DONTEVEN, "execvp");
    execvp(argv[0], argv);
    FAIL_MSG("Starting process %s for stream %s failed: %s", argv[0], streamname.c_str(), strerror(errno));
    _exit(42);
  }

  unsigned int waiting = 0;
  while (!streamAlive(streamname) && ++waiting < 40){
    Util::wait(250);
  }
  return streamAlive(streamname);
}

uint8_t Util::getStreamStatus(const std::string & streamname){
  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, NAME_BUFFER_SIZE, SHM_STREAM_STATE, streamname.c_str());
  IPC::sharedPage streamStatus(pageName, 1, false, false);
  if (!streamStatus){return STRMSTAT_OFF;}
  return streamStatus.mapped[0];
}

