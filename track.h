/** Class representing single channel audio track **/
#pragma once

#include <jack/jack.h>

class Track
{
    public:
        int nMonMix; //Monitor gain level 0 - 100
        bool bMuteA; //True if A-leg is muted
        bool bMuteB; //True if B-Leg is muted
        bool bRecording; //True if recording - mute output
        jack_port_t* pSourcePort = NULL; //Pointer to Jack source port

        /** Get the channel A mix down value of sample for this channel
        *   @param  fValue Sample value
        *   @return <i>float</i> Antenuated value
        */
        float Mix(float fValue)
        {
            if((bMuteA && bMuteB) || bRecording || 0 == nMonMix)
                return 0;
            else
                return nMonMix * fValue / 100;
        }
};
