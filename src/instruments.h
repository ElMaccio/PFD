#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <pigpio.h>
#include <cstring>
#include <iostream>

//#define BUFFER_SIZE 64

class Instruments {
    public:
        enum Type{
            BatteryVoltage,
            Altitude,
            Airspeed,
            Temperature1,
            Temperature2,
            Roll,
            Pitch,
            NumInstruments
        };

        struct InstrumentData
        {
            uint8_t idWithInop;
            float value;
        } __attribute__((packed));

        int initInstruments();
        void readInstruments();
        float getInstrumentValue(int type) const;
        void cleanup();

        private:
            int spi_handle;
            InstrumentData instruments[NumInstruments];
            char tx_buf[NumInstruments * 5];
            char rx_buf[NumInstruments * 5];
            int bufferSize;
};
