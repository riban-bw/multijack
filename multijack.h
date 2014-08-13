#pragma once
#include <jack/jack.h>
#include <ncurses.h>
#include <string>
#include <vector>

class Track;

//Constants
static const int DEFAULT_SAMPLERATE = 44100; //Samples per second
static const int SAMPLESIZE         = 4; //Quantity of bytes in each sample (4 for 32-bit)
static const int PERIOD_SIZE        = 128; //Number of frames in each period (128 samples at 441000 takes approx 3ms)
static const int MAX_TRACKS         = 16; //Quantity of mono tracks
static const int RECORD_LATENCY     = 3000; //microseconds of record latency
static const int REPLAY_LATENCY     = 3000; //microseconds of record latency
static const int MENU_HEAD          = 0; //Position of head position in menu
static const int MENU_SIZE          = 20; //Position of file size in menu
static const int MENU_TC            = 32; //Position of transport control in menu
static const int MENU_FORMAT        = 39; //Position of data format in menu
static const int MENU_PROJECT       = 57; //Position of project name in menu
//Transport control states
static const int TC_STOPPED     = 0;
static const int TC_ROLLING     = 1;
static const int TC_STOPPING    = 2;
static const int TC_STOP        = 3; //Request transport stop
static const int TC_START       = 4; //Request transport start
//Colours
static const int WHITE_RED      = 1;
static const int BLACK_GREEN    = 2;
static const int WHITE_BLUE     = 3;
static const int RED_BLACK      = 4;
static const int WHITE_MAGENTA  = 5;
//Ports to connect
static const unsigned int PORT_NONE = 0;
static const unsigned int PORT_A    = 1;
static const unsigned int PORT_B    = 2;
static const unsigned int PORT_BOTH = 3;

/** Structure representing RIFF WAVE format chunk header (without id or size, i.e. 8 bytes smaller) **/
struct WaveHeader
{
    uint16_t nAudioFormat; //1=PCM
    uint16_t nNumChannels; //Number of channels in project
    uint32_t nSampleRate; //Samples per second - expect SAMPLERATE
    uint32_t nByteRate; //nSamplrate * nNumChannels * nBitsPerSample / 8
    uint16_t nBlockAlign; //nNumChannels * nBitsPerSample / 8 (bytes for one sample of all chanels)
    uint16_t nBitsPerSample; //Expect 16
};

jack_port_t* g_pPortInputA;
jack_port_t* g_pPortInputB;
jack_port_t* g_pPortPlaybackA;
jack_port_t* g_pPortPlaybackB;
jack_client_t* g_pJackClient;
WINDOW* g_pWindowRouting; //Pointer to ncurses window

/** @brief  Handle Jack process events
*   @param  nFrames Quantity of frames to process
*   @param  pArgs Pointer to a structure of arguments (not used)
*/
int OnJackProcess(jack_nframes_t nFrames, void* pArgs);

/** @brief  Handle Jack sync (transport state / position) events
*   @param  nState Transport state (JackTransportStopped | JackTransportRolling | JackTransportLooping | JackTransportStarting)
*   @param  pPos Pointer to position structure
*   @param  pArgs Pointer to a structure of arguments (not used)
*   @return <i>int</i> 0 until ready to roll
*/
int OnJackSync(jack_transport_state_t nState, jack_position_t* pPos, void* pArgs);

void OnJackTimebase(jack_transport_state_t nState, jack_nframes_t nFrames, jack_position_t *pPos, int nNewPos, void *pArgs);

/** @brief  Handle Jack latency change event
*   @param  latencyMode Latency callback mode (JackCaptureLatency | JackPlaybackLatency)
*   @param  pArgs Pointer to a structure of arguments (not used)
*/
void OnJackLatency(jack_latency_callback_mode_t latencyMode, void* pArgs);

/** @brief  Handle Jack shutdown event
*   @param  pArgs Pointer to a structure of arguments (not used)
*/
void OnJackShutdown(void *pArgs);

/** @brief  Handle Jack buffer size change event
*   @param  nFrames Size of buffer in frames
*   @param  pArgs Pointer to a structure of arguments (not used)
*/
int OnJackBufferChange(jack_nframes_t nframes, void *pArgs);

/** @brief  Shows the menu
*/
void ShowMenu();

/** @brief  Update display with current head position
*/
void ShowHeadPosition();

/** @brief  Handle keyboard input
*/
void HandleControl();

/** @brief  Opens WAVE file and reads header
*/
bool OpenFile();

/** @brief  Writes a RIFF header to currently open file
*   @param  nWaveSize Quantity of bytes in wave data
*   @param  nChannels Quantity of tracks
*/
void WriteHeader(unsigned int nWaveSize, unsigned int nChannels);

/** @brief  Write a 16-bit, little-endian word to a char buffer
*   @param  pBuffer Buffer to write to
*   @param  nWord 16-bit word to write
*/
void SetLE16(char* pBuffer, uint16_t nWord);

/** @brief  Write a 32-bit, little-endian word to a char buffer
*   @param  pBuffer Buffer to write to
*   @param  nWord 32-bit word to write
*/
void SetLE32(char* pBuffer, uint32_t nWord);

/** @brief  Close WAVE file
*/
void CloseFile();

/** @brief  Move play head to new postion
*   @param  nPosition New position of playhead in frames relative to start
*/
void SetPlayHead(int nPosition);

/** @brief  Load a project
*   @param  sName Project name
*   @return <i>bool</i> True on succuess
*/
bool LoadProject(std::string sName);

/** @brief  Save the current project
*   @param  sName Project name
*   @return <i>bool</i> True on succuess
*/
bool SaveProject(std::string sName = "");

/** @brief  Updates the project length and updates display
*/
void UpdateLength();

/** @brief  Removes all Jack sources and creates one per track
*/
void CreateJackSources();

/** @brief  Connect track to playback port
*   @param  nTrack Index of track to connect
*   @param  nPorts Which ports to connect (PORT_NONE | PORT_A | PORT_B | PORT_BOTH)
*   @note   Use PORT_NONE to disconnect
*/
void ConnectPlayback(unsigned int nTrack, unsigned int nPorts = PORT_BOTH);

/** @brief  Disconnect track from playback port
*   @param  nTrack Index of track to connect
*   @param  nPorts Which ports to disconect (PORT_A | PORT_B | PORT_BOTH)
*/
void DisconnectPlayback(unsigned int nTrack, unsigned int nPorts = PORT_BOTH);

/** @brief  Record audio to selected tracks
*   @param  nFrames Quantity of frames to write
*   @return <i>bool</i> True on success
*/
bool Record(jack_nframes_t nFrames);

//Global variables
jack_nframes_t g_nCaptureLatency; //Numbers of frames of capture latency
jack_nframes_t g_nPlaybackLatency; //Numbers of frames of playback latency
jack_nframes_t g_nSamplerate; //Quantity of frames per second
jack_nframes_t g_nRecordOffset; //Quantity of frames offset between record head and play head
//unsigned int g_nChannels; //Quantity of tracks
unsigned int g_nSelectedTrack; //Currently selected track
int g_nRecA; //Number of track primed to record A-leg input
int g_nRecB; //Number of track primed to record B-leg input
int g_nTransport; //Transport status
int g_nFrameSize; //Quantity of bytes in each frame
static int g_nPeriodSize; //Period size - size of all samples in each period (sample size x quantity of channels x PERIOD_SIZE)
long g_lLastFrame; //Last frame
long g_lHeadPos; //Quantity of frames from start of current head position
bool g_bRecordEnabled; //True if recording
bool g_bLoop; //True to continue main loop
int g_fdWave; //File descriptor of wave file
std::string g_sPath; //Project path
std::string g_sProject; //Project name
off_t g_offStartOfData; //Offset of data in wave file
off_t g_offEndOfData; //Offset of end of data in wave file (end of file)
char* g_pSilence; //Pointer to one period of silent samples
jack_default_audio_sample_t* g_pReadBuffer; //Buffer to hold data read from file
unsigned long g_lDebug; //Misc debug variable
std::vector<jack_port_t*> g_vJackSourcePorts; //Vector of source ports, one per track
std::vector<Track*> g_vTracks; //Vector of pointers to instances of tracks
