#include <cstdio>
#include "SparkFun_VEML6030_Ambient_Light_Sensor.h"
int main() {
    TwoWire bus;
    SparkFun_Ambient_Light sensor(0x48);
    if(!sensor.begin(bus)) return 1;
    for(unsigned failure=1;failure<=3;++failure) {
        bus.readTransaction=0;bus.failTransaction=failure;bus.shortTransaction=0;
        if(sensor.readLight()!=0 || sensor.lastReadOk()) { std::printf("FAIL failed transaction %u became valid lux\n",failure);return 1; }
        bus.readTransaction=0;bus.failTransaction=0;bus.shortTransaction=failure;
        if(sensor.readLight()!=0 || sensor.lastReadOk()) { std::printf("FAIL short transaction %u became valid lux\n",failure);return 1; }
        bus.readTransaction=0;bus.shortTransaction=0;
        if(sensor.readLight()==0 || !sensor.lastReadOk()) { std::puts("FAIL normal read did not recover");return 1; }
    }
    std::puts("VEML read validity: data/gain/integration NACK + short-read failures cannot become lux; next read recovers.");
    return 0;
}
