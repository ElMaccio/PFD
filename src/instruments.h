#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <pigpio.h>
#include <string>
#include <cstring>
#include <iostream>

//#define BUFFER_SIZE 64

class Instruments {
    public:
        enum Type{
            Null,
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
            uint8_t id;
            float value;
            bool isInop;
        } __attribute__((packed));

        int initInstruments();
        void readInstruments();
        float getInstrumentValue(int type) const;
        bool getInstrumentInop(int type) const;
        std::string getInstrumentName(int type) const;
        void cleanup();

        static inline const std::string instrument_names[NumInstruments - 1] = {
            "Battery Voltage",
            "Altitude",
            "Airspeed",
            "Temperature 1",
            "Temperature 2",
            "Roll",
            "Pitch"
        };

        static inline const std::string instrument_units[NumInstruments - 1] = {
            "mV",
            "m",
            "m/s",
            "C",
            "C",
            "deg",
            "deg"
        };

        private:
            int spi_handle;
            InstrumentData instruments[NumInstruments - 1];
            char tx_buf[(NumInstruments - 1) * 5];
            char rx_buf[(NumInstruments - 1) * 5];
            int bufferSize;

            void setIdWithInop(InstrumentData& data, uint8_t idWithInop);
};
