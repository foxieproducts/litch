#pragma once
#include <cstdint>

// Native observation boundary, not a model of MMIO offsets or USB packets.
struct usb_in_endpoint_t {
    volatile uint32_t diepctl = 0U;
    volatile uint32_t diepint = 0U;
    volatile uint32_t dieptsiz = 0U;
    volatile uint32_t dtxfsts = 0U;
};
struct usb_dev_t {
    volatile uint32_t gintsts = 0U;
    volatile uint32_t gintmsk = 0U;
    volatile uint32_t gahbcfg = 0U;
    volatile uint32_t dcfg = 0U;
    volatile uint32_t dctl = 0U;
    volatile uint32_t diepmsk = 0U;
    volatile uint32_t daint = 0U;
    volatile uint32_t daintmsk = 0U;
    volatile uint32_t dtknqr4_fifoemptymsk = 0U;
    usb_in_endpoint_t in_ep_reg[7]{};
};
extern usb_dev_t USB0;
