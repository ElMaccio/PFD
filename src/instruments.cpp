#include "instruments.h"

int Instruments::initInstruments() {
    if (gpioInitialise() < 0) {
        fprintf(stderr, "pigpio initialization failed\n");
        return 1;
    }

    spi_handle = spiOpen(0, 1000000, 0);
    if (spi_handle < 0) {
        fprintf(stderr, "Failed to open SPI channel\n");
        gpioTerminate();
        return 1;
    }

    bufferSize = (NumInstruments - 1) * 5;

    for(int i=0;i<bufferSize;i++) 
            tx_buf[i]=0xFF;        

    std::cout << "Instruments initialized." << std::endl;

    return 0;
}

void Instruments::readInstruments() {
    spiXfer(spi_handle, tx_buf, rx_buf, bufferSize);

    for (int i = 0; i < NumInstruments - 1; i++)
    {
            setIdWithInop(instruments[i], rx_buf[i * 5]);

            memcpy(&instruments[i].value, &rx_buf[i * 5 + 1], sizeof(float));
            
            if(i == Instruments::BatteryVoltage - 1)
                instruments[i].value *= 0.25f;
            
            //printf("%s, ID: 0x%02X, Value: %f %s, Inop: %d\n", instrument_names[i].c_str(), instruments[i].id, instruments[i].value, instrument_units[i], instruments[i].isInop);
    }
}

float Instruments::getInstrumentValue(int type) const {
    if (type < 1 || type >= NumInstruments) return 0.0f;
    return instruments[type - 1].value;
}

bool Instruments::getInstrumentInop(int type) const {
    if (type < 1 || type >= NumInstruments) return false;
    return instruments[type - 1].isInop;
}

std::string Instruments::getInstrumentName(int type) const {
    if (type < 1 || type >= NumInstruments) return "";
        return instrument_names[type - 1];
}

std::string Instruments::getInstrumentUnit(int type) const
{
    if (type < 1 || type >= NumInstruments) return "";
        return instrument_units[type - 1];
}

void Instruments::cleanup() {
    spiClose(spi_handle);
    gpioTerminate();
}

void Instruments::setIdWithInop(InstrumentData &data, uint8_t idWithInop) {
    data.id = idWithInop & 0x7F;

    if(data.id == 0)
    {
        data.isInop = true;
        return;
    }
    data.isInop = (idWithInop & 0x80) != 0;
}
