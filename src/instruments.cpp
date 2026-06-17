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

    bufferSize = NumInstruments * 5;

    for(int i=0;i<bufferSize;i++) 
            tx_buf[i]=0xFF;        

    std::cout << "Instruments initialized." << std::endl;

    return 0;
}

void Instruments::readInstruments() {
    spiXfer(spi_handle, tx_buf, rx_buf, bufferSize);

    for (int i = 0; i < NumInstruments; i++)
    {
            setIdWithInop(instruments[i], rx_buf[i * 5]);

            memcpy(&instruments[i].value, &rx_buf[i * 5 + 1], sizeof(float));

            //printf("ID: 0x%02X, Value: %f\n", instruments[i].idWithInop, instruments[i].value);
    }
}

float Instruments::getInstrumentValue(int type) const {
    if (type < 0 || type >= NumInstruments) return 0.0f;
    return instruments[type].value;
}

float Instruments::getInstrumentInop(int type) const {
    if (type < 0 || type >= NumInstruments) return false;
    return instruments[type].isInop;
}

void Instruments::cleanup() {
    spiClose(spi_handle);
    gpioTerminate();
}

void Instruments::setIdWithInop(InstrumentData &data, uint8_t idWithInop) {
    data.id = idWithInop & 0x7F;
    data.isInop = (idWithInop & 0x80) != 0;
}
