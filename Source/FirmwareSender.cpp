/*
  g++ -std=c++11 -ISource -IJuceLibraryCode Source/FirmwareSender.cpp Source/sysex.c JuceLibraryCode/modules/juce_core/juce_core.cpp JuceLibraryCode/modules/juce_audio_basics/juce_audio_basics.cpp JuceLibraryCode/modules/juce_audio_devices/juce_audio_devices.cpp JuceLibraryCode/modules/juce_events/juce_events.cpp -lpthread -ldl -lX11 -lasound
*/
#ifndef _WIN32
#include <unistd.h>
#endif
#include <stdint.h>
#include <math.h>
#include "JuceHeader.h"
#include "OpenWareMidiControl.h"
#include "crc32.h"
#include "sysex.h"
#include "MidiStatus.h"

#define MESSAGE_SIZE 8
#define DEFAULT_BLOCK_SIZE (248-MESSAGE_SIZE)
#define DEFAULT_BLOCK_DELAY 20 // wait in milliseconds between sysex messages

bool quiet = false;

class CommandLineException : public std::exception {
private:
  juce::String cause;
public:
  CommandLineException(juce::String c) : cause(c) {}
  juce::String getCause() const {
    return cause;
  }
  const char* what() const noexcept {
    return getCause().toUTF8();
  }
};

class FirmwareSender {
private:
  bool running = false;
  bool verbose = false;
  juce::ScopedPointer<MidiOutput> midiout;
  juce::ScopedPointer<File> fileout;
  juce::ScopedPointer<File> input;
  juce::ScopedPointer<OutputStream> out;
  int blockDelay = DEFAULT_BLOCK_DELAY;
  int blockSize = DEFAULT_BLOCK_SIZE;
  int storeSlot = -1;
  bool doRun = false;
  bool doFlash = false;
  unsigned int flashChecksum;
public:
  void listDevices(const StringArray& names){
    for(int i=0; i<names.size(); ++i)
      std::cout << i << ": " << names[i] << std::endl;
  }

  MidiOutput* openMidiOutput(const String& name){
    MidiOutput* output = NULL;    
    StringArray outputs = MidiOutput::getDevices();
    for(int i=0; i<outputs.size(); ++i){
      if(outputs[i].trim().matchesWildcard(name, true)){
	if(verbose)
	  std::cout << "opening MIDI output " << outputs[i] << std::endl;
	output = MidiOutput::openDevice(i);
	break;
      }
    }
    if(output != NULL)
      output->startBackgroundThread();
    return output;
  }

  void send(MemoryBlock& block){
    send((unsigned char*)block.getData(), block.getSize());
  }

  void send(unsigned char* data, int size){
    if(verbose)
      std::cout << "sending " << std::dec << size << " bytes" << std::endl;
    if(out != NULL){
      out->writeByte(SYSEX);
      out->write(data, size);
      out->writeByte(SYSEX_EOX);
      out->flush();
    }
    if(midiout != NULL)
      midiout->sendMessageNow(juce::MidiMessage::createSysExMessage(data, size));
  }

  void usage(){
    std::cerr << getApplicationName() << std::endl 
	      << "usage:" << std::endl
	      << "-h or --help\tprint this usage information and exit" << std::endl
	      << "-l or --list\tlist available MIDI ports and exit" << std::endl
	      << "-in FILE\tinput FILE" << std::endl
	      << "-out DEVICE\tsend output to MIDI interface DEVICE" << std::endl
	      << "-save FILE\twrite output to FILE" << std::endl
	      << "-store NUM\tstore in slot NUM" << std::endl
	      << "-run\t\tstart patch after upload" << std::endl
	      << "-flash NUM\tflash firmware with checksum NUM" << std::endl
	      << "-d NUM\t\tdelay for NUM milliseconds between blocks" << std::endl
	      << "-s NUM\t\tlimit SysEx messages to NUM bytes" << std::endl
	      << "-q or --quiet\treduce status output" << std::endl
	      << "-v or --verbose\tincrease status output" << std::endl
      ;
  }

  void configure(int argc, char* argv[]) {
    for(int i=1; i<argc; ++i){
      juce::String arg = juce::String(argv[i]);
      if(arg.compare("-h") == 0 || arg.compare("--help") == 0 ){
	usage();
	throw CommandLineException(juce::String::empty);
      }else if(arg.compare("-q") == 0 || arg.compare("--quiet") == 0 ){
	quiet = true;
      }else if(arg.compare("-v") == 0 || arg.compare("--verbose") == 0 ){
	verbose = true;
      }else if(arg.compare("-l") == 0 || arg.compare("--list") == 0){
	std::cout << "MIDI input devices:" << std::endl;
	listDevices(MidiInput::getDevices());
	std::cout << "MIDI output devices:" << std::endl;
	listDevices(MidiOutput::getDevices());
	throw CommandLineException(juce::String::empty);
      }else if(arg.compare("-d") == 0 && ++i < argc){
	blockDelay = juce::String(argv[i]).getIntValue();
      }else if(arg.compare("-s") == 0 && ++i < argc){
	blockSize = juce::String(argv[i]).getIntValue() - MESSAGE_SIZE;
      }else if(arg.compare("-store") == 0 && ++i < argc){
	storeSlot = juce::String(argv[i]).getIntValue();
      }else if(arg.compare("-run") == 0){
	doRun = true;
      }else if(arg.compare("-flash") ==0 && ++i < argc){
	doFlash = true;
	flashChecksum = juce::String(argv[i]).getHexValue32();
	std::cout << "Sending FLASH command with checksum " << std::hex << flashChecksum << std::endl;	
      }else if(arg.compare("-in") == 0 && ++i < argc){
	juce::String name = juce::String(argv[i]);
	input = new juce::File(name);
	if(!input->exists())
	  throw CommandLineException("No such file: "+name);
      }else if(arg.compare("-out") == 0 && ++i < argc){
	juce::String name = juce::String(argv[i]);
	midiout = openMidiOutput(name);
	if(midiout == NULL)
	  throw CommandLineException("MIDI device not available: "+name);  
      }else if(arg.compare("-save") == 0 && ++i < argc){
	juce::String name = juce::String(argv[i]);
	fileout = new juce::File(name);
	fileout->deleteFile();
	fileout->create();
      }else{
	usage();
	throw CommandLineException(juce::String::empty);
      }
    }
    if(input == NULL || (midiout == NULL && fileout == NULL)){
      usage();
      throw CommandLineException(juce::String::empty);
    }
    if(midiout == NULL && blockDelay == DEFAULT_BLOCK_DELAY)
      blockDelay = 0;
  }

  void run(){
    running = true;
    if(!quiet){
      std::cout << "Sending file " << input->getFileName() << std::endl; 
      if(midiout != NULL)
	std::cout << "\tto MIDI output" << std::endl; 
      if(fileout != NULL)
	std::cout << "\tto SysEx file " << fileout->getFullPathName() << std::endl;       
    }
    const char header[] =  { MIDI_SYSEX_MANUFACTURER, MIDI_SYSEX_DEVICE, SYSEX_FIRMWARE_UPLOAD };
    int binblock = (int)floor(blockSize*7/8);
    // int sysblock = (int)ceil(binblock*8/7);

    juce::ScopedPointer<InputStream> in = input->createInputStream();
    if(fileout != NULL)
      out = fileout->createOutputStream();

    int packageIndex = 0;
    MemoryBlock block;
    block.append(header, sizeof(header));
    encodeInt(block, packageIndex++);
    unsigned char* buffer = (unsigned char*)alloca(binblock*sizeof(unsigned char));
    unsigned char* sysex = (unsigned char*)alloca(blockSize*sizeof(unsigned char));
    int size = input->getSize(); // amount of data, excluding checksum
    encodeInt(block, size);
    // send first message with index and length
    send(block);

    uint32_t checksum = 0;
    for(int i=0; i < size && running;){
      block = MemoryBlock();
      block.append(header, sizeof(header));
      encodeInt(block, packageIndex++);
      int len = in->read(buffer, binblock);
      checksum = crc32(buffer, len, checksum);
      i += len;
      if(verbose)
	std::cout << "preparing " << std::dec << len;
      len = data_to_sysex(buffer, sysex, len);
      if(verbose)
	std::cout << "/" << len << " bytes binary/sysex (total " << 
	  i << " of " << size << " bytes)" << std::endl;
      block.append(sysex, len);
      send(block);
      if(blockDelay > 0)
	juce::Time::waitForMillisecondCounter(juce::Time::getMillisecondCounter()+blockDelay);
    }

    if(running){
      // last block: package index and checksum
      block = MemoryBlock();
      block.append(header, sizeof(header));
      encodeInt(block, packageIndex++);
      encodeInt(block, checksum);
      send(block);
      if(blockDelay > 0)
	juce::Time::waitForMillisecondCounter(juce::Time::getMillisecondCounter()+blockDelay);

      if(!quiet)
	std::cout << "checksum 0x" << std::hex << checksum << std::endl;

      if(storeSlot >= 0){
	if(!quiet)
	  std::cout << "store slot " << std::hex << storeSlot << std::endl;
	const char tailer[] =  { MIDI_SYSEX_MANUFACTURER, MIDI_SYSEX_DEVICE, SYSEX_FIRMWARE_STORE };
	block = MemoryBlock();
	block.append(tailer, sizeof(tailer));
	encodeInt(block, storeSlot);
	send(block);
      }else if(doRun){
	const char tailer[] =  { MIDI_SYSEX_MANUFACTURER, MIDI_SYSEX_DEVICE, SYSEX_FIRMWARE_RUN };
	block = MemoryBlock();
	block.append(tailer, sizeof(tailer));
	send(block);
      }else if(doFlash){
	const char tailer[] =  { MIDI_SYSEX_MANUFACTURER, MIDI_SYSEX_DEVICE, SYSEX_FIRMWARE_FLASH };
	block = MemoryBlock();
	block.append(tailer, sizeof(tailer));
	encodeInt(block, flashChecksum);
	send(block);
      }
    }
    stop();
  }

  void encodeInt(MemoryBlock& block, uint32_t data){
    uint8_t in[4];
    uint8_t out[5];
    in[3] = (uint8_t)data & 0xff;
    in[2] = (uint8_t)(data >> 8) & 0xff;
    in[1] = (uint8_t)(data >> 16) & 0xff;
    in[0] = (uint8_t)(data >> 24) & 0xff;
    int len = data_to_sysex(in, out, 4);
    if(len != 5)
      throw CommandLineException("Error in sysex conversion"); 
    block.append(out, len);
  }

  void stop(){
    if(midiout != NULL)
      midiout->stopBackgroundThread();
    if(out != NULL)
      out->flush();
  }

  void shutdown(){
    running = false;
  }

  juce::String getApplicationName(){
    return "FirmwareSender";
  }
};
 
FirmwareSender* app = NULL;

#ifndef _WIN32
void sigfun(int sig){
  if(!quiet)
    std::cout << "shutting down" << std::endl;
  if(app != NULL)
    app->shutdown();
  (void)signal(SIGINT, SIG_DFL);
}
#endif

int main(int argc, char* argv[]) {
#ifndef _WIN32
  (void)signal(SIGINT, sigfun);
#endif
  int status = 0;
  app = new FirmwareSender();
  try{
    app->configure(argc, argv);
    app->run();
  }catch(const std::exception& exc){
    std::cerr << exc.what() << std::endl;
    status = -1;
  }
  delete app;
  return status;
}
