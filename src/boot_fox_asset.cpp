#include "modules/boot_fox_asset.hpp"

namespace modules::boot_fox_asset {
// Canonical app/assets/branding/foxie-fox.png SHA256:
// c4701cfbe33a518092e4876468daaecf2acf1c41cc140113dde25fc7d1659925.
// Pixel sampling: brightest source feature (alpha >=192), black matte,
// within each V3.3 PCB emitter's Voronoi cell; fit aspect ratio, no recoloring.
// PCB c4990dc65aa9f986, 25 mm pitch, LED1..256: Left/Top/Right/Bottom.
// Payload RGB+mask SHA256: 06f7c628cc02fc5775a9382cf882ec083ca3b2174a248baf060da59c7ec79ce0.
const std::array<Rgb, 256U> PIXELS{{
    {0U, 0U, 0U}, {0U, 0U, 0U}, {254U, 204U, 0U}, {73U, 0U, 224U},
    {254U, 204U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {254U, 204U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {254U, 204U, 0U}, {254U, 204U, 0U},
    {251U, 201U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {58U, 0U, 149U}, {255U, 105U, 0U}, {0U, 0U, 0U},
    {255U, 105U, 0U}, {254U, 204U, 0U}, {255U, 203U, 0U}, {255U, 103U, 0U},
    {254U, 204U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {254U, 204U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {254U, 204U, 0U}, {254U, 204U, 0U},
    {254U, 204U, 0U}, {254U, 204U, 0U}, {255U, 105U, 0U}, {255U, 103U, 0U},
    {255U, 103U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {255U, 103U, 0U}, {255U, 103U, 0U}, {0U, 0U, 0U},
    {255U, 103U, 0U}, {255U, 203U, 0U}, {73U, 0U, 224U}, {255U, 103U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {255U, 103U, 0U}, {254U, 201U, 0U}, {255U, 103U, 0U}, {255U, 103U, 0U},
    {255U, 103U, 0U}, {255U, 103U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {251U, 201U, 0U}, {253U, 201U, 0U},
    {254U, 204U, 0U}, {73U, 0U, 224U}, {0U, 0U, 0U}, {254U, 204U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {73U, 0U, 224U}, {254U, 204U, 0U}, {252U, 201U, 0U},
    {251U, 201U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {251U, 201U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {249U, 100U, 0U}, {0U, 0U, 0U}, {255U, 105U, 0U}, {255U, 105U, 0U},
    {255U, 103U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {255U, 105U, 0U},
    {0U, 0U, 0U}, {254U, 204U, 0U}, {252U, 201U, 0U}, {0U, 0U, 0U},
    {254U, 204U, 0U}, {251U, 201U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {251U, 201U, 0U}, {254U, 204U, 0U}, {0U, 0U, 0U},
    {252U, 201U, 0U}, {254U, 204U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {255U, 105U, 0U}, {255U, 105U, 0U},
    {255U, 105U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {255U, 105U, 0U},
    {0U, 0U, 0U}, {255U, 103U, 0U}, {255U, 103U, 0U}, {0U, 0U, 0U},
    {255U, 103U, 0U}, {255U, 105U, 0U}, {255U, 105U, 0U}, {0U, 0U, 0U},
    {255U, 105U, 0U}, {0U, 0U, 0U}, {255U, 105U, 0U}, {255U, 103U, 0U},
    {255U, 105U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {255U, 105U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {255U, 105U, 0U}, {255U, 105U, 0U},
    {255U, 105U, 0U}, {0U, 0U, 0U}, {255U, 105U, 0U}, {255U, 103U, 0U},
    {255U, 105U, 0U}, {255U, 105U, 0U}, {78U, 0U, 223U}, {0U, 0U, 0U},
    {75U, 0U, 224U}, {255U, 103U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {255U, 103U, 0U}, {255U, 103U, 0U}, {0U, 0U, 0U},
    {255U, 105U, 0U}, {255U, 105U, 0U}, {0U, 0U, 0U}, {255U, 105U, 0U},
    {0U, 0U, 0U}, {255U, 105U, 0U}, {255U, 105U, 0U}, {255U, 105U, 0U},
    {255U, 103U, 0U}, {255U, 103U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {255U, 103U, 0U}, {255U, 103U, 0U}, {0U, 0U, 0U},
    {255U, 103U, 0U}, {255U, 103U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
    {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U}, {0U, 0U, 0U},
}};
const std::array<uint8_t, 32U> YELLOW_MASK{{
    0x94U, 0x00U, 0x00U, 0x1cU, 0x60U, 0x09U, 0xc0U, 0x03U, 0x00U, 0x02U, 0x20U, 0x00U, 0xc0U, 0x09U, 0xc0U, 0x09U, 0x00U, 0x36U, 0x36U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
}};
static_assert(sizeof(PIXELS) + sizeof(YELLOW_MASK) == 800U);

bool IsYellow(const uint16_t index) {
    return index < PIXELS.size() && (YELLOW_MASK[index / 8U] & (1U << (index % 8U))) != 0U;
}
} // namespace modules::boot_fox_asset
