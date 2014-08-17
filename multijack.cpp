/** Simple multitrack recorder with Jack interface
*   2 channel input - record from 1 or 2 inputs to any track
*   2 channel output - mixdown for monitoring, select which output(s) to route each track to
*   Single multichannel WAVE file may be imported to DAW for editing
*   Acts like linear multitrack tape recorder
*/

///@todo Transport navigation causes short play of audio, e.g. goto home

#include "multijack.h"
#include "track.h"
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h> //provides control of terminal - set raw mode
#include <string>
#include <alsa/asoundlib.h>
#include <ncurses.h> //provides user interface
#include <iostream>
#include <sys/types.h> //provides lseek

using namespace std;

int OnJackProcess(jack_nframes_t nFrames, void* pArgs)
{
    //!@todo Optimse process code
    if(TC_STOPPED == g_nTransport)
        return 0; //Not rolling so don't process any audio
    else if(TC_STOPPING == g_nTransport)
    {
        //Transport stop requested so silence all channels then set transport to stop
        //Already faded out last sample (see code below)
        for(unsigned int nChan = 0; nChan < g_vTracks.size(); ++nChan)
        {
            jack_default_audio_sample_t* pOut = (jack_default_audio_sample_t*)(jack_port_get_buffer(g_vJackSourcePorts[nChan], nFrames));
            if(pOut)
                memset(pOut, 0, nFrames * sizeof(jack_default_audio_sample_t));
        }
        jack_transport_stop(g_pJackClient);
        g_nTransport = TC_STOPPED;
        return 0;
    }
    if(!g_bRecordEnabled && g_lHeadPos > g_lLastFrame - (2 * nFrames))
        g_nTransport = TC_STOP; //Fade out penultimate frame and don't play last frame (which may be too short to fade)
    //Rolling so read from wave file
    int nRead = read(g_fdWave, g_pReadBuffer, nFrames * g_nFrameSize); //!@todo pre-read from file outside this callback
    if((nRead > 0))// && (g_lHeadPos <= g_lLastFrame - nFrames)) //!@todo extra code to soften stop at end of file stops end of file being reached, e.g. can't play from start again (do we want to?)
    {
        //Iterate through input buffer one frame at a time, adding gain-adjusted value to output buffer for each track
        unsigned int nReadFrames = nRead / g_nFrameSize;
        for(unsigned int nFrame = 0; nFrame < nReadFrames; ++nFrame)
        {
            for(unsigned int nChan = 0; nChan < g_vTracks.size(); ++nChan)
            {
                jack_default_audio_sample_t fSample = g_pReadBuffer[nFrame * g_vTracks.size() + nChan];
                jack_default_audio_sample_t* pOut = (jack_default_audio_sample_t*)(jack_port_get_buffer(g_vJackSourcePorts[nChan], nReadFrames));
                if(TC_STOP == g_nTransport)
                    pOut[nFrame] = (nFrames - nFrame) * g_vTracks[nChan]->Mix(fSample) / nFrames; //Fade out last frame to reduce click on stop
                else if(TC_START == g_nTransport)
                    pOut[nFrame] = nFrame * g_vTracks[nChan]->Mix(fSample) / nFrames; //Fade in first frame to reduce click on start
                else
                    pOut[nFrame] = g_vTracks[nChan]->Mix(fSample);
            }
        }
        g_lHeadPos += nReadFrames;
    }
    if(TC_STOP == g_nTransport)
        g_nTransport = TC_STOPPING;
    if(TC_START == g_nTransport)
    {
        g_nTransport = TC_ROLLING;
        jack_transport_start(g_pJackClient);
    }

    //Past end of file so either stop if we are playing or extend file if we are recording
    //!@todo Extend file outside this callback
    if(g_lHeadPos >= g_lLastFrame)
    {
        if(g_bRecordEnabled)
        {
            //Recording so extend file by one frame
            g_lLastFrame += nFrames;
            g_lHeadPos += nFrames;
            g_offEndOfData = lseek(g_fdWave, nFrames * g_nFrameSize - 1, SEEK_END) + 1; //This sets end of data marker but positions write cursor one byte early
            char pCharZero[1] = {0}; //null value to write to file
            write(g_fdWave, pCharZero, 1); //Writes last byte to file, extending file and moving write cursor to end - hole is populated with null (silent) data
        }
        else
            g_nTransport = TC_STOPPING; //Not recording so request stop
    }
    ShowHeadPosition();

    Record(nFrames);

	return 0;
}

int OnJackSync(jack_transport_state_t nState, jack_position_t* pPos, void* pArgs)
{
    mvprintw(20,0, "OnJackSync state = %d count = %ld", nState, ++g_lDebug);
    //!@todo Handle external transport and position changes
    switch(nState)
    {
        case JackTransportStarting:
            g_nTransport = TC_START;
            break;
        case JackTransportRolling:
            g_nTransport = TC_ROLLING;
            break;
        case JackTransportStopped:
            g_nTransport = TC_STOP;
            g_lHeadPos = pPos->frame;
            break;
        default:
            break;
    }
    return 1; //Always ready to roll
}

void OnJackTimebase(jack_transport_state_t nState, jack_nframes_t nFrames, jack_position_t *pPos, int nNewPos, void *pArgs)
{
    mvprintw(20,0, "OnJackTimebase state = %d count = %ld", nState, ++g_lDebug);
    switch(nState)
    {
        case JackTransportStarting:
            g_nTransport = TC_START;
            break;
        case JackTransportRolling:
            g_nTransport = TC_ROLLING;
            break;
        case JackTransportStopped:
            g_nTransport = TC_STOP;
            g_lHeadPos = pPos->frame;
            break;
        default:
            break;
    }
}

void OnJackLatency(jack_latency_callback_mode_t latencyMode, void* pArgs)
{
    jack_latency_range_t latencyRange;
    jack_port_get_latency_range(g_pPortInputA, latencyMode, &latencyRange);
    if(latencyMode == JackCaptureLatency)
        g_nCaptureLatency = latencyRange.max;
    else
        g_nPlaybackLatency = latencyRange.max;
    g_nRecordOffset = g_nCaptureLatency + g_nPlaybackLatency;
}

void OnJackShutdown(void* pArgs)
{
    //!@todo Flag Jack closed
	g_pJackClient = NULL;
}

int OnJackBufferChange(jack_nframes_t nFrames, void *pArgs)
{
    delete[] g_pReadBuffer;
    g_pReadBuffer = new jack_default_audio_sample_t[nFrames * g_vTracks.size()];
    return 0;
}

int main(int argc, char *argv[])
{
    g_lDebug = 0;
	g_nCaptureLatency = 0;
	g_nPlaybackLatency = 0;
    g_nTransport = TC_STOPPED;
    g_bRecordEnabled = false;
    g_nSelectedTrack = 0;
    g_nRecA = -1; //Deselect A-channel recording
    g_nRecB = -1; //Deselect B-channel recording
    g_bRunning = true; //Main program loop flag - loop if true
    g_fdWave = -1;
    g_pSilence = NULL;
    g_pReadBuffer = NULL;
    g_sPath = "/media/multitrack/"; //!@todo replace this absolute path
    g_pJackClient = NULL;
    g_nJackConnectAttempt = 0;

    //Initialise ncurses
    initscr();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    start_color();
    init_pair(WHITE_RED, COLOR_WHITE, COLOR_RED);
    init_pair(BLACK_GREEN, COLOR_BLACK, COLOR_GREEN);
    init_pair(WHITE_BLUE, COLOR_WHITE, COLOR_BLUE);
    init_pair(RED_BLACK, COLOR_RED, COLOR_BLACK);
    init_pair(WHITE_MAGENTA, COLOR_WHITE, COLOR_MAGENTA);
    attron(COLOR_PAIR(WHITE_MAGENTA));
    mvprintw(0, 0, "                                             ");
    attroff(COLOR_PAIR(WHITE_MAGENTA));
    g_pWindowRouting = newwin(MAX_TRACKS, 40, 1, 0);
    refresh();

    //Set stdin to non-blocking
    termios flags;
    if(tcgetattr(fileno(stdin), &flags) < 0)
    {
        /* handle error */
        cerr << "Failed to get terminal attributes" << endl;
    }
    flags.c_lflag &= ~ICANON; // set raw (unset canonical modes)
    flags.c_cc[VMIN] = 0; // i.e. min 1 char for blocking, 0 chars for non-blocking
    flags.c_cc[VTIME] = 0; // block if waiting for char
    if(tcsetattr(fileno(stdin), TCSANOW, &flags) < 0)
    {
        cerr << "Failed to set terminal attributes" << endl;
        Quit(1);
    }

	/* keep running until stopped by the user */
	while(g_bRunning)
    {
        HandleControl();
        while(!g_pJackClient)
        {
            ConnectJack(); //!@todo Not connecting to playback on startup
            sleep(1);
            HandleControl();
            if(!g_bRunning)
                Quit();
        }
        usleep(1000);
    }
    Quit();
}

void Quit(int nError)
{
    if(TC_ROLLING == g_nTransport)
    {
        //Currently playing so need to stop
        g_nTransport = TC_STOP;
        g_bRecordEnabled = false;
        if(g_nRecA > -1)
            g_vTracks[g_nRecA]->bRecording = false;
        if(g_nRecB > -1)
            g_vTracks[g_nRecB]->bRecording = false;
        UpdateLength();
        //!@todo Could use while(TC_STOPPED != g_nTransport) but may never end if Jack server is not running
        usleep(100000); //Wait for soft stop to complete (fade out audio over one period)
    }
    SaveProject();
    CloseFile();
    delete[] g_pSilence;
    delete[] g_pReadBuffer;
    endwin(); //End ncurses
    if(g_pJackClient)
        jack_client_close(g_pJackClient);
	exit(nError);
}

void ShowMenu()
{
    for(unsigned int i = 0; i < g_vTracks.size(); ++i)
    {
        if(i == g_nSelectedTrack)
            wattron(g_pWindowRouting, COLOR_PAIR(WHITE_BLUE));
        mvwprintw(g_pWindowRouting, i, 0, "Track %02d: ", i + 1);
        wattroff(g_pWindowRouting, COLOR_PAIR(WHITE_BLUE));
        if((int)i == g_nRecA)
        {
            wattron(g_pWindowRouting, COLOR_PAIR(WHITE_RED));
            wprintw(g_pWindowRouting, "REC-A ");
            wattroff(g_pWindowRouting, COLOR_PAIR(WHITE_RED));
        }
        else
            wprintw(g_pWindowRouting, "      ");
        if((int)i == g_nRecB)
        {
            wattron(g_pWindowRouting, COLOR_PAIR(WHITE_RED));
            wprintw(g_pWindowRouting, "REC-B ");
            wattroff(g_pWindowRouting, COLOR_PAIR(WHITE_RED));
        }
        else
            wprintw(g_pWindowRouting, "      ");
        if(g_vTracks[i]->bMuteA && g_vTracks[i]->bMuteB)
        {
            wattron(g_pWindowRouting, COLOR_PAIR(RED_BLACK));
            wprintw(g_pWindowRouting, " MUTE   ");
            wattroff(g_pWindowRouting, COLOR_PAIR(RED_BLACK));
        }
        else
        {
            char aChar[8];
            sprintf(aChar, "% 4d %s%s", g_vTracks[i]->nMonMix, g_vTracks[i]->bMuteA?" ":"L", g_vTracks[i]->bMuteB?" ":"R");
            wprintw(g_pWindowRouting, " %s ", aChar);
        }
    }
    wrefresh(g_pWindowRouting);
    switch(g_nTransport)
    {
        case TC_STOPPED:
        case TC_STOPPING:
        case TC_STOP:
            if(g_bRecordEnabled)
                attron(COLOR_PAIR(WHITE_RED));
            else
                attron(COLOR_PAIR(BLACK_GREEN));
            mvprintw(0, MENU_TC, " STOP ");
            if(g_bRecordEnabled)
                attroff(COLOR_PAIR(WHITE_RED));
            else
                attroff(COLOR_PAIR(BLACK_GREEN));
            break;
        case TC_ROLLING:
        case TC_START:
            if(g_bRecordEnabled)
                attron(COLOR_PAIR(WHITE_RED));
            else
                attron(COLOR_PAIR(BLACK_GREEN));
            mvprintw(0, MENU_TC, " ROLL ");
            if(g_bRecordEnabled)
                attroff(COLOR_PAIR(WHITE_RED));
            else
                attroff(COLOR_PAIR(BLACK_GREEN));
            break;
    }
    refresh();
}

void ShowHeadPosition()
{
    attron(COLOR_PAIR(WHITE_MAGENTA));
    unsigned int nMinutes = g_lHeadPos / g_nSamplerate / 60;
    unsigned int nSeconds = (g_lHeadPos - nMinutes * g_nSamplerate * 60) / g_nSamplerate;
    unsigned int nMillis = (g_lHeadPos - (nMinutes * 60 + nSeconds) * g_nSamplerate) * 1000 / 44100;
    mvprintw(0, MENU_HEAD, "Position: %02d:%02d.%03d/", nMinutes, nSeconds, nMillis);
    attroff(COLOR_PAIR(WHITE_MAGENTA));
}

void HandleControl()
{
    int nInput = getch();
    switch(nInput)
    {
        case 'q':
            //Quit
            //!@todo Confirm quit
            g_bRunning = false;
            break;
        case 'o':
            //Open project
            //!@todo Implement open project
            break;
        case KEY_DOWN:
            //Select next track
            if(++g_nSelectedTrack >= g_vTracks.size())
                g_nSelectedTrack = g_vTracks.size() - 1;
            break;
        case KEY_UP:
            //Select previou track
            if(g_nSelectedTrack > 0)
                --g_nSelectedTrack;
            break;
        case KEY_RIGHT:
            //Increase monitor level
            if(g_vTracks[g_nSelectedTrack]->nMonMix < 100)
                ++g_vTracks[g_nSelectedTrack]->nMonMix;
            break;
        case KEY_LEFT:
            //Decrease monitor level
            if(g_vTracks[g_nSelectedTrack]->nMonMix > 0)
                --g_vTracks[g_nSelectedTrack]->nMonMix;
            break;
        case KEY_SRIGHT:
            //Set monitor to full level
            g_vTracks[g_nSelectedTrack]->nMonMix = 100;
            break;
        case KEY_SLEFT:
            //Set monitor to zero level
            g_vTracks[g_nSelectedTrack]->nMonMix = 0;
            break;
        case 'l':
            //Toggle A-leg mute
            if(g_vTracks[g_nSelectedTrack]->bMuteA)
            {
                g_vTracks[g_nSelectedTrack]->bMuteA = false;
                ConnectPlayback(g_nSelectedTrack, PORT_A);
            }
            else
            {
                g_vTracks[g_nSelectedTrack]->bMuteA = true;
                DisconnectPlayback(g_nSelectedTrack, PORT_A);
            }
            break;
        case 'r':
            //Toggle B-leg mute
            if(g_vTracks[g_nSelectedTrack]->bMuteB)
            {
                g_vTracks[g_nSelectedTrack]->bMuteB = false;
                ConnectPlayback(g_nSelectedTrack, PORT_B);
            }
            else
            {
                g_vTracks[g_nSelectedTrack]->bMuteB = true;
                DisconnectPlayback(g_nSelectedTrack, PORT_B);
            }
            break;
        case 'a':
            //Toggle record from A
            if(g_nRecA == (int)g_nSelectedTrack)
            {
                g_vTracks[g_nRecA]->bRecording = false;
                g_nRecA = -1;
            }
            else
            {
                if(g_nRecA > -1)
                    g_vTracks[g_nRecA]->bRecording = false;
                g_nRecA = g_nSelectedTrack;
                g_vTracks[g_nRecA]->bRecording = true;
            }
            break;
        case 'b':
            //Toggle record from B
            if(g_nRecB == (int)g_nSelectedTrack)
            {
                g_vTracks[g_nRecB]->bRecording = false;
                g_nRecB = -1;
            }
            else
            {
                if(g_nRecB > -1)
                    g_vTracks[g_nRecB]->bRecording = false;
                g_nRecB = g_nSelectedTrack;
                g_vTracks[g_nRecB]->bRecording = true;
            }
            break;
        case 'm':
            //Toggle monitor mute (both legs)
            if(g_vTracks[g_nSelectedTrack]->bMuteA && g_vTracks[g_nSelectedTrack]->bMuteB)
            {
               g_vTracks[g_nSelectedTrack]->bMuteA = false;
               g_vTracks[g_nSelectedTrack]->bMuteB = false;
            }
            else
            {
               g_vTracks[g_nSelectedTrack]->bMuteA = true;
               g_vTracks[g_nSelectedTrack]->bMuteB = true;
            }
            ConnectPlayback(g_nSelectedTrack, g_vTracks[g_nSelectedTrack]->bMuteA?PORT_NONE:PORT_BOTH);
            break;
        case 'M':
            //Toggle all monitor mute
            {
                bool bMute = !g_vTracks[g_nSelectedTrack]->bMuteA;
                for(unsigned int i = 0; i < g_vTracks.size(); ++i)
                {
                    g_vTracks[i]->bMuteA = bMute;
                    g_vTracks[i]->bMuteB = bMute;
                    ConnectPlayback(i, bMute?PORT_NONE:PORT_BOTH);
                }
            }
            break;
        case ' ':
            //Start / Stop
            switch(g_nTransport)
            {
                case TC_STOPPED:
                    //Currently stopped so need to open files and interfaces and start
                    g_nTransport = TC_START;
                    //!@todo Configure whether auto return to zero when playing from end of track
                    if(!g_bRecordEnabled && g_lHeadPos >= g_lLastFrame)
                        g_lHeadPos = 0;
                    SetPlayHead(g_lHeadPos);
                    break;
                case TC_ROLLING:
                    //Currently playing so need to stop
                    g_nTransport = TC_STOP;
                    g_bRecordEnabled = false;
                    if(g_nRecA > -1)
                        g_vTracks[g_nRecA]->bRecording = false;
                    if(g_nRecB > -1)
                        g_vTracks[g_nRecB]->bRecording = false;
                    UpdateLength();
                    break;
            }
            break;
        case 'G':
            //Toggle record mode
            if(g_bRecordEnabled)
            {
                if(g_nRecA > -1)
                    g_vTracks[g_nRecA]->bRecording = false;
                if(g_nRecB > -1)
                    g_vTracks[g_nRecB]->bRecording = false;
            }
            g_bRecordEnabled = !g_bRecordEnabled;
            break;
        case KEY_HOME:
            //Go to home position
            SetPlayHead(0);
            break;
        case KEY_END:
            //Go to end of track
            SetPlayHead(g_lLastFrame);
            break;
        case ',':
            //Back 1 seconds
            SetPlayHead(g_lHeadPos - 1 * g_nSamplerate);
            break;
        case '.':
            //Forward 1 seconds
            SetPlayHead(g_lHeadPos + 1 * g_nSamplerate);
            break;
        case '<':
            //Back 10 seconds
            SetPlayHead(g_lHeadPos - 10 * g_nSamplerate);
            break;
        case '>':
            //Forward 10 seconds
            SetPlayHead(g_lHeadPos + 10 * g_nSamplerate);
            break;
        case 'e':
            //Clear errors
//            g_nUnderruns = 0;
//            g_nOverruns = 0;
//            g_nRecUnderruns = 0;
            move(18, 0);
            clrtoeol();
            move(19, 0);
            clrtoeol();
            break;
        case 'z':
            //Debug
            break;
        default:
            return; //Avoid updating menu if invalid keypress
    }
    ShowMenu();
}

bool OpenFile()
{
    // Expect header to be 12 + 24 + 8 = 44
	//**Open file**
	if(g_fdWave < 0)
    {
        //File not open
        string sFilename = g_sPath;
        sFilename.append(g_sProject);
        sFilename.append(".wav");
        g_fdWave = open(sFilename.c_str(), O_RDWR | O_CREAT, 0644);
        if(g_fdWave <= 0)
        {
            cerr << "Unable to open or create file" << sFilename << " - error " << errno << endl;
            return false;
        }

        //**Read RIFF headers**
        char pBuffer[12];
        if((read(g_fdWave, pBuffer, 12) < 12) || (0 != strncmp(pBuffer, "RIFF", 4)) || (0 != strncmp(pBuffer + 8, "WAVE", 4)))
        {
            //Invalid file so create a WAVE file with 4 seconds of silence
            g_nSamplerate = jack_get_sample_rate(g_pJackClient); //!@todo Handle different samplerate to project (warn and resolve?)
            if(0 == g_nSamplerate)
                g_nSamplerate = DEFAULT_SAMPLERATE;
            size_t nWaveSize = g_nSamplerate * MAX_TRACKS * sizeof(jack_default_audio_sample_t) * 4;
            WriteHeader(nWaveSize, MAX_TRACKS);
            unsigned char pSilentBuffer[nWaveSize];
            memset(pSilentBuffer, 0, nWaveSize);
            pwrite(g_fdWave, pSilentBuffer, nWaveSize, 44);
            ftruncate(g_fdWave, 44 + nWaveSize);
            lseek(g_fdWave, 12, SEEK_SET);
        }

        char pWaveBuffer[sizeof(WaveHeader)];
        WaveHeader* pWaveHeader = (WaveHeader*)pWaveBuffer;

        //Look for chuncks
        while(read(g_fdWave, pBuffer, 8) == 8) //read ckID and cksize
        {
            char sId[5] = {0,0,0,0,0}; //buffer for debug output only
            strncpy(sId, pBuffer, 4); //chunk ID is first 32-bit word
            uint32_t nSize = (uint32_t)*(pBuffer + 4); //chunk size is second 32-bit word

    //        cerr << endl << "Found chunk " << sId << " of size " << nSize << endl;
            if(0 == strncmp(pBuffer, "fmt ", 4)) //chunk ID is first 32-bit word
            {
                //Found format chunk
                if(read(g_fdWave, pWaveBuffer, sizeof(pWaveBuffer)) < (int)sizeof(pWaveBuffer))
                {
                    cerr << "Too small for WAVE header" << endl;
//                    CloseReplay();
                    return false;
                }
                for(unsigned int nTrack = 0; nTrack < pWaveHeader->nNumChannels; ++nTrack)
                    g_vTracks.push_back(new Track());
                if(g_vTracks.size() > MAX_TRACKS)
                {
                    //!@todo handle too many tracks, e.g. ask whether to delete extra tracks
                }
                CreateJackSources();
                g_nSamplerate = pWaveHeader->nSampleRate;
                if(0 == g_nSamplerate)
                    g_nSamplerate = DEFAULT_SAMPLERATE;
                g_nFrameSize = g_vTracks.size() * sizeof(jack_default_audio_sample_t);
                if(jack_get_sample_rate(g_pJackClient) != g_nSamplerate)
                    attron(COLOR_PAIR(WHITE_RED));
                else
                    attron(COLOR_PAIR(WHITE_MAGENTA));
                mvprintw(0, MENU_FORMAT, " % 6dHz ", pWaveHeader->nSampleRate);
                attroff(COLOR_PAIR(WHITE_MAGENTA));
                lseek(g_fdWave, nSize - sizeof(pWaveBuffer), SEEK_CUR); //ignore other parameters
            }
            else if(0 == strncmp(pBuffer, "data ", 4))
            {
                //Aligned with start of data so must have read all header
                g_offStartOfData = lseek(g_fdWave, 0, SEEK_CUR);
                g_offEndOfData = lseek(g_fdWave, 0, SEEK_END);

                if(g_offStartOfData != 44)
                {
                    mvprintw(18, 0, "Importing file - please wait...");
                    attron(COLOR_PAIR(COLOR_RED));
                    mvprintw(19, 0, "                                    ");
                    attroff(COLOR_PAIR(COLOR_RED));
                    refresh();
                    //Use minimal RIFF header - write new header, move wave data then truncate file
                    off_t nWaveSize = g_offEndOfData - g_offStartOfData;
                    WriteHeader(nWaveSize, g_vTracks.size());
                    char pData[512];
                    off_t offRead = g_offStartOfData;
                    off_t offWrite = 44;
                    int nRead;
                    int nProgress = 0;
                    while((nRead = pread(g_fdWave, pData, sizeof(pData), offRead)) > 0)
                    {
                        pwrite(g_fdWave, pData, nRead, offWrite);
                        offWrite += nRead;
                        offRead += nRead;
                        int nProgressTemp = 100 * offRead / nWaveSize;
                        if(nProgressTemp != nProgress)
                        {
                            nProgress = nProgressTemp;
                            mvprintw(18, 32, "% 2d%%", nProgress);
                            attron(COLOR_PAIR(COLOR_GREEN));
                            mvprintw(19, nProgress / 2.77, " ");
                            attroff(COLOR_PAIR(COLOR_GREEN));
                            refresh();
                        }
                    }
                    ftruncate(g_fdWave, 44 + nWaveSize);
                    g_offStartOfData = 44;
                    move(18, 0);
                    clrtoeol();
                    move(19, 0);
                    clrtoeol();
                    refresh();
                }

                g_offEndOfData = lseek(g_fdWave, 0, SEEK_END);
                g_lLastFrame = (g_offEndOfData - g_offStartOfData) / (g_nFrameSize);
                return true;
            }
            else
                lseek(g_fdWave, nSize, SEEK_CUR); //Not found desired chunk so seek to next chunk
        }
        cerr << "Failed to get WAVE header";
    }
    return false;
}

void CreateJackSources()
{
    if(!g_pJackClient)
        return;
    //Disconnect existing ports
    for(vector<jack_port_t*>::iterator it = g_vJackSourcePorts.begin(); it != g_vJackSourcePorts.end(); ++it)
        jack_port_disconnect(g_pJackClient, *it);
    g_vJackSourcePorts.clear();
    for(unsigned int i = 1; i <= g_vTracks.size(); ++i)
    {
        char sName[9];
        memset(sName, 0, 9);
        sprintf(sName, "Track %02u", i);
        jack_port_t* pPort = jack_port_register(g_pJackClient, sName, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if(pPort)
            g_vJackSourcePorts.push_back(pPort);
        else
            cerr << "Failed to created source port " << i << endl;
        g_vTracks[i - 1]->pSourcePort = pPort;
    }
}

void WriteHeader(unsigned int nWaveSize, unsigned int nChannels)
{
    if(g_fdWave <= 0)
        return;
    //Use minimal RIFF header - write new header, move wave data then truncate file
    char pHeader[36];
    strncpy(pHeader, "RIFF", 4);
    SetLE32(pHeader + 4, nWaveSize + 36); //size of RIFF chunck
    strncpy(pHeader + 8, "WAVE", 4);
    strncpy(pHeader + 12, "fmt ", 4); //start of format chunk
    SetLE32(pHeader + 16, 16); //size of format chunck
    SetLE16(pHeader + 20, 3); //Audio format = IEEE float
    SetLE16(pHeader + 22, nChannels); //Number of channels
    SetLE32(pHeader + 24, g_nSamplerate);
    SetLE32(pHeader + 28, g_nSamplerate * nChannels * sizeof(jack_default_audio_sample_t)); //Sample rate
    SetLE16(pHeader + 32, nChannels * sizeof(jack_default_audio_sample_t)); //Block align == frame size
    SetLE16(pHeader + 34, sizeof(jack_default_audio_sample_t) * 8); //Bits per sample
    pwrite(g_fdWave, pHeader, sizeof(pHeader), 0);
    strncpy(pHeader, "data", 4);
    SetLE32(pHeader + 4, nWaveSize);
    pwrite(g_fdWave, pHeader, 8, 36);
}

/** Write a 16-bit, little-endian word to a char buffer */
void SetLE16(char* pBuffer, uint16_t nWord)
{
    *pBuffer = char(nWord & 0xFF);
    *(pBuffer + 1) = char((nWord >> 8) & 0xFF);
}

/** Write a 32-bit, little-endian word to a char buffer */
void SetLE32(char* pBuffer, uint32_t nWord)
{
    *pBuffer = char(nWord & 0xFF);
    *(pBuffer + 1) = char((nWord >> 8) & 0xFF);
    *(pBuffer + 2) = char((nWord >> 16) & 0xFF);
    *(pBuffer + 3) = char((nWord >> 24) & 0xFF);
}

void CloseFile()
{
    if(g_fdWave > 0)
    {
        //Write RIFF chunck length
        char pBuffer[4];
        SetLE32(pBuffer, g_offEndOfData - 8);
        pwrite(g_fdWave, pBuffer, 4, 4);
        close(g_fdWave);
    }
    g_fdWave = -1;
    if(TC_ROLLING == g_nTransport)
        g_nTransport = TC_STOP; //!@todo Can we fade out after closing file?
    for(vector<Track*>::iterator it = g_vTracks.begin(); it != g_vTracks.end(); ++it)
        delete *it;
    g_vTracks.clear();
}

void SetPlayHead(int nPosition)
{
    if(g_bRecordEnabled && TC_ROLLING == g_nTransport)
        return; //Don't allow shuttling when recording
    g_lHeadPos = nPosition;
    if(g_lHeadPos < 0)
        g_lHeadPos = 0;
    if(g_lHeadPos > g_lLastFrame)
        g_lHeadPos = g_lLastFrame;
    if(g_fdWave > 0)
        lseek(g_fdWave, g_offStartOfData + g_lHeadPos * g_nFrameSize, SEEK_SET);
    jack_transport_locate(g_pJackClient, nPosition);
    ShowHeadPosition();
}

bool LoadProject(string sName)
{
    //Project consists of sName.wav and sName.cfg
    //Close existing WAVE file and open new one
    CloseFile();
    attron(COLOR_PAIR(WHITE_MAGENTA));
    move(0, MENU_FORMAT);
    clrtoeol();
    attroff(COLOR_PAIR(WHITE_MAGENTA));
    g_sProject = sName;
    if(!OpenFile())
        return false;
    attron(COLOR_PAIR(WHITE_MAGENTA));
    mvprintw(0, MENU_PROJECT, "Project: %s", sName.c_str());
    attroff(COLOR_PAIR(WHITE_MAGENTA));

    //Get configuration
    string sConfig = g_sPath;
    sConfig.append(sName);
    sConfig.append(".cfg");
    FILE *pFile = fopen(sConfig.c_str(), "r");
    if(pFile)
    {
        char pLine[256];
        while(fgets(pLine, sizeof(pLine), pFile))
        {
            if(strnlen(pLine, sizeof(pLine)) < 5)
                continue;
            unsigned int nChannel = (pLine[0] - '0') * 10 + (pLine[1] - '0');
            if(nChannel >= 0 && nChannel < g_vTracks.size())
            {
                switch(pLine[2])
                {
                    case 'V':
                        g_vTracks[nChannel]->nMonMix = atoi(pLine + 4);
                        break;
                    case 'L':
                        //Route  / Mute A
                        g_vTracks[nChannel]->bMuteA = (pLine[4] != '1');
                        break;
                    case 'R':
                        //Route  / Mute B
                        g_vTracks[nChannel]->bMuteB = (pLine[4] != '1');
                        break;
                }
                if(g_vTracks[nChannel]->bMuteA)
                    DisconnectPlayback(nChannel, PORT_A);
                else
                    ConnectPlayback(nChannel, PORT_A);
                if(g_vTracks[nChannel]->bMuteB)
                    DisconnectPlayback(nChannel, PORT_B);
                else
                    ConnectPlayback(nChannel, PORT_B);
            }
            if(0 == strncmp(pLine, "Pos=", 4))
                g_lHeadPos = atoi(pLine + 4); //Set transport position
        }
        fclose(pFile);
    }
    SetPlayHead(g_lHeadPos);
    g_nPeriodSize = g_nFrameSize * PERIOD_SIZE; //!@todo Use Jack period size
    //Create new silent period
    delete[] g_pSilence;
    g_pSilence = new char[g_nPeriodSize];
    memset(g_pSilence, 0, g_nPeriodSize);
    //Create new read buffer
    delete[] g_pReadBuffer;
    g_pReadBuffer = new jack_default_audio_sample_t[g_nPeriodSize];
    UpdateLength();
    return true;
}

bool SaveProject(std::string sName)
{
    std::string sConfig = g_sPath;
    if(sName == "")
    {
        sConfig.append(g_sProject);
    }
    else
    {
        sConfig.append(sName);
        string sCpCmd = "cp ";
        sCpCmd.append(g_sPath);
        sCpCmd.append(g_sProject);
        sCpCmd.append(".wav");
        sCpCmd.append(" ");
        sCpCmd.append(g_sPath);
        sCpCmd.append(sName);
        sCpCmd.append(".wav");
        system(sCpCmd.c_str());
        g_sProject = sName;
    }
    sConfig.append(".cfg");
    FILE *pFile = fopen(sConfig.c_str(), "w+");
    if(pFile)
    {
        char pBuffer[32];
        fputs("# This configuration file is completely overwritten each time the project is saved\n", pFile);
        fputs("# Do not manually edit this file whilst multijack is using this project.\n\n", pFile);
        for(unsigned int i = 0; i < g_vTracks.size(); ++i)
        {
            memset(pBuffer, 0, sizeof(pBuffer));
            sprintf(pBuffer, "%02dV=%d\n", i, g_vTracks[i]->nMonMix);
            fputs(pBuffer, pFile);
            memset(pBuffer, 0, sizeof(pBuffer));
            sprintf(pBuffer, "%02dL=%s",i, g_vTracks[i]->bMuteA?"0\n":"1\n");
            fputs(pBuffer, pFile);
            memset(pBuffer, 0, sizeof(pBuffer));
            sprintf(pBuffer, "%02dR=%s",i, g_vTracks[i]->bMuteB?"0\n":"1\n");
            fputs(pBuffer, pFile);
        }
        memset(pBuffer, 0, sizeof(pBuffer));
        sprintf(pBuffer, "Pos=%ld\n", g_lHeadPos);
        fputs(pBuffer , pFile);

        fclose(pFile);
        return true;
    }
    return false;
}

void UpdateLength()
{
    if(0 == g_nFrameSize)
        return;
    g_lLastFrame = (g_offEndOfData - g_offStartOfData) / g_nFrameSize;
    attron(COLOR_PAIR(WHITE_MAGENTA));
    unsigned int nMinutes = g_lLastFrame / g_nSamplerate / 60;
    unsigned int nSeconds = (g_lLastFrame - nMinutes * g_nSamplerate * 60) / g_nSamplerate;
    unsigned int nMillis = (g_lLastFrame - (nMinutes * 60 + nSeconds) * g_nSamplerate) * 1000 / 44100;
    mvprintw(0, MENU_SIZE, "%02d:%02d.%03d ", nMinutes, nSeconds, nMillis);
    attroff(COLOR_PAIR(WHITE_MAGENTA));
}

void ConnectPlayback(unsigned int nTrack, unsigned int nPorts)
{
    if(nTrack >= g_vTracks.size())
        return;
    char pCharPort[19];
    sprintf(pCharPort, "multijack:Track %02d", nTrack + 1);
    if(PORT_NONE == nPorts)
    {
        jack_disconnect(g_pJackClient, pCharPort, jack_port_name(g_pPortPlaybackA));
        jack_disconnect(g_pJackClient, pCharPort, jack_port_name(g_pPortPlaybackB));
        return;
    }
    if(PORT_A & nPorts)
        jack_connect(g_pJackClient, pCharPort, jack_port_name(g_pPortPlaybackA));
    if(PORT_B & nPorts)
        jack_connect(g_pJackClient, pCharPort, jack_port_name(g_pPortPlaybackB));
}

void DisconnectPlayback(unsigned int nTrack, unsigned int nPorts)
{
    if(nTrack >= g_vTracks.size())
        return;
    char pCharPort[19];
    sprintf(pCharPort, "multijack:Track %02d", nTrack + 1);
    if(PORT_A & nPorts)
        jack_disconnect(g_pJackClient, pCharPort, jack_port_name(g_pPortPlaybackA));
    if(PORT_B & nPorts)
        jack_disconnect(g_pJackClient, pCharPort, jack_port_name(g_pPortPlaybackB));
}

bool Record(jack_nframes_t nFrames)
{
    if(TC_ROLLING != g_nTransport)
        return false; //Can't record if we are not rolling
    if(!g_bRecordEnabled)
        return false; //Don't record if we are not in record mode
    if(g_fdWave <= 0)
        return false; //WAVE file not open so nothing to record to
    if((-1 == g_nRecA) && (-1 == g_nRecB))
        return false; //No record channels primed
    if(g_lHeadPos < g_nRecordOffset)
        return true; //Record head not past start of file

    //!@todo Move extend file code to here?

    jack_default_audio_sample_t* pInA = (jack_default_audio_sample_t*)(jack_port_get_buffer(g_pPortInputA, nFrames));
    jack_default_audio_sample_t* pInB = (jack_default_audio_sample_t*)(jack_port_get_buffer(g_pPortInputB, nFrames));

    //Write samples to file
    off_t offRewrite = g_offStartOfData + (g_lHeadPos - g_nRecordOffset) * g_nFrameSize;
    ssize_t nRead = pread(g_fdWave, g_pReadBuffer, nFrames * g_nFrameSize, offRewrite);
    if(nRead != nFrames * g_nFrameSize)
        return false; //Failed to read frame of data
    for(jack_nframes_t nFrame = 0; nFrame < nFrames; ++nFrame)
    {
        if(-1 != g_nRecA)
            g_pReadBuffer[nFrame * g_vTracks.size() + g_nRecA] = pInA[nFrame];
        if(-1 != g_nRecB)
            g_pReadBuffer[nFrame * g_vTracks.size() + g_nRecB] = pInB[nFrame];
    }
    pwrite(g_fdWave, g_pReadBuffer, nRead, offRewrite);
    return true;
}

bool ConnectJack()
{
	//open a client connection to the JACK server
	jack_options_t options = JackNoStartServer;
	jack_status_t nStatus;
	const char *pCharServerName = NULL; //Pointer to name of Jack server
    const char** as_ports; //array of pointers to c-strings used to hold list of port names
    if(!g_pJackClient)
        g_pJackClient = jack_client_open("multijack", options, &nStatus, pCharServerName);
	if(!g_pJackClient)
    {
		if(nStatus & JackServerFailed)
        attron(COLOR_PAIR(WHITE_RED));
        mvprintw(20, 0, " Disconnected from JACK - attempting to recover... % 3u ", ++g_nJackConnectAttempt);
        attroff(COLOR_PAIR(WHITE_RED));
        wrefresh(g_pWindowRouting);
		return false;
	}
	g_nSamplerate = jack_get_sample_rate(g_pJackClient); //!@todo Is it useful to get samplerate here? Should we (attempt to) set Jack samplerate after opening file?

    //Assign Jack callback handler functions
    //Set callback to handle Jack process (something needs to be done)
	jack_set_process_callback(g_pJackClient, OnJackProcess, 0);
	//Set callback to handle Jack slow sync (transport) events
	jack_set_sync_callback(g_pJackClient, OnJackSync, 0); //!@todo Does not handled STOP from Jack transport
//    jack_set_timebase_callback(g_pJackClient, 1, OnJackTimebase, 0); //!@todo This does not work
    //Set callback to handle Jack shutdown
    jack_on_shutdown(g_pJackClient, OnJackShutdown, 0);
    //Set callback to handle latency changes
    jack_set_latency_callback(g_pJackClient, OnJackLatency, 0);
    //Set callback to handle Jack buffer size change
    jack_set_buffer_size_callback(g_pJackClient, OnJackBufferChange, 0);

    //Create buffer to hold samples read from file
    g_pReadBuffer = new jack_default_audio_sample_t[jack_get_buffer_size(g_pJackClient) * g_vTracks.size()];

	//Create capture ports
	g_pPortInputA = jack_port_register(g_pJackClient, "Input A", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	g_pPortInputB = jack_port_register(g_pJackClient, "Input B", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	if(!g_pPortInputA || !g_pPortInputB)
    {
        cerr << "Error - cannot register Jack ports" << endl;
		return false;
	}
	//Find playback ports (expect 2)
	as_ports = jack_get_ports(g_pJackClient, NULL, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical | JackPortIsInput);
	if(as_ports == NULL)
    {
		cerr << "No physical playback as_ports" << endl;
		return false;
	}
	int nPort = 0;
	while(as_ports[++nPort])
        ;
    if(nPort < 2)
    {
        fprintf(stderr, "Error - insufficient playback ports - require 2, found %d", nPort);
        free(as_ports);
        return false;
    }
	//!@todo Handle different playback port configuration, e.g. when monitor ports are not first two
 	g_pPortPlaybackA = jack_port_by_name(g_pJackClient, as_ports[0]);
 	g_pPortPlaybackB = jack_port_by_name(g_pJackClient, as_ports[1]);
	free(as_ports);

	if(jack_activate(g_pJackClient))
    {
		fprintf(stderr, "Error - cannot activate Jack client\n");
		return false;
	}

    LoadProject("default");
    ShowMenu();

    //Connect capture ports
	as_ports = jack_get_ports(g_pJackClient, NULL, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical | JackPortIsOutput);
	if(as_ports == NULL)
    {
		fprintf(stderr, "Error - no physical capture ports available\n");
		return false;
	}
	nPort = 0;
	while(as_ports[++nPort])
        ; //!@todo There must be a better way to deduce quantity of ports
    if(nPort < 2)
    {
        fprintf(stderr, "Error - insufficient capture ports - require 2, found %d", nPort);
        free(as_ports);
        return false;
    }
    if(jack_connect(g_pJackClient, as_ports[0], jack_port_name(g_pPortInputA)))
    {
        fprintf (stderr, "Cannot connect input port A\n");
    }
    if(jack_connect(g_pJackClient, as_ports[1], jack_port_name(g_pPortInputB)))
    {
        fprintf (stderr, "Cannot connect input port B\n");
    }
	free(as_ports);

    move(20, 0);
    clrtoeol();
    g_nJackConnectAttempt = 0;
	return true;
}
