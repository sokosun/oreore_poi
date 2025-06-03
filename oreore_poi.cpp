// - WS2812B LED detects following patterns as commands
//   = Reset |_____________(80+us)___________| :
//   = 0     |‾‾‾|_________| (0.3us, 0.9us)    :
//   = 1     |‾‾‾‾‾‾|______| (0.6us, 0.6us)    :
// - Each WS2812B LED requires 24bit RGB value to change colors
//   = MSB first & GRB sequence (G7, G6, ... G1, R7, R6, ..., R1, B7, B6, ... B1)

// 5760bit (720bytes) to refresh 240 LEDs
// 2MB = Refresh 2900 times = 960cm

#include <array>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#define DMA0 0
#define LENGTH 80 // the number of LEDs on each strip

const uint64_t DEFAULT_PERIOD_us = 2500; // 400Hz
const uint64_t POLL_GPIO_us = 10000;

#include "ws2812.pio.h"
#include "bluewave.h"
#include "symbol.h"
#include "rainbow.h"
#include "singleline.h"

//-----------------------------------------
// Utilities

void sleep_us_since(const uint64_t us, const uint32_t since){
    const auto curr = time_us_32();
    const auto spent = curr - since;
    if(us < spent){
        return;
    }

    sleep_us(us - spent);
}

//-----------------------------------------
// GPIO related

// Pin Assignment
//
// D0/GPIO26:  PIO[0]
// D1/GPIO27:  PIO[1]
// D2/GPIO28:  PIO[2]
// D3/GPIO5:   -
// D4/GPIO6:   -
// D5/GPIO7:   PushSW
// D6/GPIO0:   DIP[0]
// D7/GPIO1:   DIP[1]
// D8/GPIO2:   DIP[2]
// D9/GPIO4:   DIP[3]
// D10/GPIO3:  DIP[4]

const uint PSW_PIN = 7;
const uint WS2812_SIGNAL0_PIN = 26;

volatile bool psw_pressed = false;

int get_dip_value(){
    const uint32_t dip0_mask = 0x00000001;
    const uint32_t dip1_mask = 0x00000002;
    const uint32_t dip2_mask = 0x00000004;
    const uint32_t dip3_mask = 0x00000010;
    const uint32_t dip4_mask = 0x00000008;
    auto raw = gpio_get_all();

    int val = 0;
    if(raw & dip0_mask){val += 1;}
    if(raw & dip1_mask){val += 2;}
    if(raw & dip2_mask){val += 4;}
    if(raw & dip3_mask){val += 8;}
    if(raw & dip4_mask){val += 16;}

    return val;
}

void psw_cbk(uint gpio, uint32_t event_mask){
    psw_pressed = true;
}

void sw_pins_init(){
    const uint pins[] = {0, 1, 2, 3, 4, PSW_PIN};
    for(auto pin : pins){
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_up(pin);
    }
    gpio_set_irq_enabled_with_callback(PSW_PIN, GPIO_IRQ_EDGE_FALL, true, psw_cbk);
}

// User LED (for debug)
const uint USR_LED_PIN = 25;
void usr_led_init(){
    gpio_init(USR_LED_PIN);
    gpio_set_dir(USR_LED_PIN, GPIO_OUT);
    gpio_put(USR_LED_PIN, 0);
}

void pio_init(){
    auto offset0 = pio_add_program(pio0, &ws2812_parallel_program);
    auto sm0 = pio_claim_unused_sm(pio0, true);
    ws2812_parallel_program_init(pio0, sm0, offset0, WS2812_SIGNAL0_PIN, 4, 800000);
    dma_channel_config dma0_conf = dma_channel_get_default_config(DMA0);
    channel_config_set_dreq(&dma0_conf, pio_get_dreq(pio0, sm0, true)); /* configure data request. true: sending data to the PIO state machine */
    channel_config_set_transfer_data_size(&dma0_conf, DMA_SIZE_32); /* data transfer size is 32 bits */
    channel_config_set_read_increment(&dma0_conf, true); /* each read of the data will increase the read pointer */
    dma_channel_configure(DMA0, &dma0_conf, &pio0->txf[sm0], NULL, 3*LENGTH, false);
}

//-----------------------------------------
// Data Format

// This program handles following data format to control LED color
//
// 1. Image
// [R0][G0][B0][R1][G1][B1] ... [R239][G239][B239]
// // [Rx|Gx|Bx] = 8bit
//
// 2. ws2812_parallel PIO
// ws2812_parallel refreshes 4 strips simultaniously
// Therefore it requires 96bit data to refresh one led for each strip
// (MSB)
// [STRIP3-G0][STRIP2-G0][STRIP1-G0][STRIP0-G0][STRIP3-G1][STRIP2-G1][STRIP1-G1][STRIP0-G1] ... [STRIP3-G7][STRIP2-G7][STRIP1-G7][STRIP0-G7]
// [STRIP3-R0][STRIP2-R0][STRIP1-R0][STRIP0-R0][STRIP3-R1][STRIP2-R1][STRIP1-R1][STRIP0-R1] ... [STRIP3-R7][STRIP2-R7][STRIP1-R7][STRIP0-R7]
// [STRIP3-B0][STRIP2-B0][STRIP1-B0][STRIP0-B0][STRIP3-B1][STRIP2-B1][STRIP1-B1][STRIP0-B1] ... [STRIP3-B7][STRIP2-B7][STRIP1-B7][STRIP0-B7]
// // [STRIPx-Gy|Ry|By] = 1bit

struct image_info {
    // static information
    // image size should be width * height * 3(RGB) bytes.
    const uint8_t * image;
    uint32_t width;     // 240
    uint32_t height;
    uint64_t period_us;
    bool loop;          // Output image repeatedly if true
    bool mirror;        // Output ABCCBA if true (image = ABC)
    bool multiline;     // Use multiline poi

    image_info(
        const uint8_t * image_,
        uint32_t width_,
        uint32_t height_,
        uint64_t period_us_ = DEFAULT_PERIOD_us,
        bool loop_ = true,
        bool mirror_ = false,
        bool multiline_ = true
    ) : image(image_), width(width_), height(height_), period_us(period_us_), loop(loop_), mirror(mirror_), multiline(multiline_) {
    }

};


#define IMG(x) (&(x[0][0]))
#define WID(x) (sizeof(x[0])/sizeof(x[0][0])/3)
#define HEI(x) (sizeof(x)/sizeof(x[0]))

image_info info_bluewave(IMG(bluewave), WID(bluewave), HEI(bluewave), DEFAULT_PERIOD_us * 3, false, false, false);
image_info info_rainbow(IMG(rainbow), WID(rainbow), HEI(rainbow));
image_info info_symbol(IMG(symbol), WID(symbol), HEI(symbol));
image_info info_red(IMG(red), WID(red), HEI(red));
image_info info_green(IMG(green), WID(green), HEI(green));
image_info info_blue(IMG(blue), WID(blue), HEI(blue));

//-----------------------------------------
// Data Handling

// 8bit R/G/B data format is converted to ws2812_parallel PIO format through parallel_lut and interleave function.
constexpr uint32_t parallel_lut[256] = {
    0x00000000, 0x10000000, 0x01000000, 0x11000000, 0x00100000, 0x10100000, 0x01100000, 0x11100000,
    0x00010000, 0x10010000, 0x01010000, 0x11010000, 0x00110000, 0x10110000, 0x01110000, 0x11110000,
    0x00001000, 0x10001000, 0x01001000, 0x11001000, 0x00101000, 0x10101000, 0x01101000, 0x11101000,
    0x00011000, 0x10011000, 0x01011000, 0x11011000, 0x00111000, 0x10111000, 0x01111000, 0x11111000,
    0x00000100, 0x10000100, 0x01000100, 0x11000100, 0x00100100, 0x10100100, 0x01100100, 0x11100100,
    0x00010100, 0x10010100, 0x01010100, 0x11010100, 0x00110100, 0x10110100, 0x01110100, 0x11110100,
    0x00001100, 0x10001100, 0x01001100, 0x11001100, 0x00101100, 0x10101100, 0x01101100, 0x11101100,
    0x00011100, 0x10011100, 0x01011100, 0x11011100, 0x00111100, 0x10111100, 0x01111100, 0x11111100,
    0x00000010, 0x10000010, 0x01000010, 0x11000010, 0x00100010, 0x10100010, 0x01100010, 0x11100010,
    0x00010010, 0x10010010, 0x01010010, 0x11010010, 0x00110010, 0x10110010, 0x01110010, 0x11110010,
    0x00001010, 0x10001010, 0x01001010, 0x11001010, 0x00101010, 0x10101010, 0x01101010, 0x11101010,
    0x00011010, 0x10011010, 0x01011010, 0x11011010, 0x00111010, 0x10111010, 0x01111010, 0x11111010,
    0x00000110, 0x10000110, 0x01000110, 0x11000110, 0x00100110, 0x10100110, 0x01100110, 0x11100110,
    0x00010110, 0x10010110, 0x01010110, 0x11010110, 0x00110110, 0x10110110, 0x01110110, 0x11110110,
    0x00001110, 0x10001110, 0x01001110, 0x11001110, 0x00101110, 0x10101110, 0x01101110, 0x11101110,
    0x00011110, 0x10011110, 0x01011110, 0x11011110, 0x00111110, 0x10111110, 0x01111110, 0x11111110,
    0x00000001, 0x10000001, 0x01000001, 0x11000001, 0x00100001, 0x10100001, 0x01100001, 0x11100001,
    0x00010001, 0x10010001, 0x01010001, 0x11010001, 0x00110001, 0x10110001, 0x01110001, 0x11110001,
    0x00001001, 0x10001001, 0x01001001, 0x11001001, 0x00101001, 0x10101001, 0x01101001, 0x11101001,
    0x00011001, 0x10011001, 0x01011001, 0x11011001, 0x00111001, 0x10111001, 0x01111001, 0x11111001,
    0x00000101, 0x10000101, 0x01000101, 0x11000101, 0x00100101, 0x10100101, 0x01100101, 0x11100101,
    0x00010101, 0x10010101, 0x01010101, 0x11010101, 0x00110101, 0x10110101, 0x01110101, 0x11110101,
    0x00001101, 0x10001101, 0x01001101, 0x11001101, 0x00101101, 0x10101101, 0x01101101, 0x11101101,
    0x00011101, 0x10011101, 0x01011101, 0x11011101, 0x00111101, 0x10111101, 0x01111101, 0x11111101,
    0x00000011, 0x10000011, 0x01000011, 0x11000011, 0x00100011, 0x10100011, 0x01100011, 0x11100011,
    0x00010011, 0x10010011, 0x01010011, 0x11010011, 0x00110011, 0x10110011, 0x01110011, 0x11110011,
    0x00001011, 0x10001011, 0x01001011, 0x11001011, 0x00101011, 0x10101011, 0x01101011, 0x11101011,
    0x00011011, 0x10011011, 0x01011011, 0x11011011, 0x00111011, 0x10111011, 0x01111011, 0x11111011,
    0x00000111, 0x10000111, 0x01000111, 0x11000111, 0x00100111, 0x10100111, 0x01100111, 0x11100111,
    0x00010111, 0x10010111, 0x01010111, 0x11010111, 0x00110111, 0x10110111, 0x01110111, 0x11110111,
    0x00001111, 0x10001111, 0x01001111, 0x11001111, 0x00101111, 0x10101111, 0x01101111, 0x11101111,
    0x00011111, 0x10011111, 0x01011111, 0x11011111, 0x00111111, 0x10111111, 0x01111111, 0x11111111
};

uint32_t interleave(const uint8_t v0, const uint8_t v1, const uint8_t v2, const uint8_t v3 = 0){
    return parallel_lut[v0] | (parallel_lut[v1] << 1) | (parallel_lut[v2] << 2) | (parallel_lut[v3] << 3);
}

void pack_parallel(uint32_t (&packet)[3*LENGTH], const uint8_t * line){
    for(int i=0;i<LENGTH;i++){
        packet[i*3]   = interleave(line[i*9+1], line[i*9+4], line[i*9+7]); // G
        packet[i*3+1] = interleave(line[i*9],   line[i*9+3], line[i*9+6]); // R
        packet[i*3+2] = interleave(line[i*9+2], line[i*9+5], line[i*9+8]); // B
    }
}

// LED assignment
//
// (normal)
//   |          [2-0]       [2-1]       [2-2]     ...  [2-79] <- line0
//   |      [1-0]       [1-1]       [1-2]    ...  [1-79]      <- line1
//   V  [0-0]       [0-1]       [0-2]   ...  [0-79]           <- line2
// swing
//
// (reverse)
// swing
//   A          [2-0]       [2-1]       [2-2]     ...  [2-79] <- line2
//   |      [1-0]       [1-1]       [1-2]    ...  [1-79]      <- line1
//   |  [0-0]       [0-1]       [0-2]   ...  [0-79]           <- line0

void pack_parallel_sft(
    uint32_t (&packet)[3*LENGTH],
    const uint8_t *line0,
    const uint8_t *line1,
    const uint8_t *line2,
    bool reverse = false
){
    if(!reverse){
        for(int i=0;i<LENGTH;i++){
            packet[i*3]   = interleave(line2[i*9+1], line1[i*9+4], line0[i*9+7]); // G
            packet[i*3+1] = interleave(line2[i*9],   line1[i*9+3], line0[i*9+6]); // R
            packet[i*3+2] = interleave(line2[i*9+2], line1[i*9+5], line0[i*9+8]); // B
        }
    }else{
        for(int i=0;i<LENGTH;i++){
            packet[i*3]   = interleave(line0[i*9+1], line1[i*9+4], line2[i*9+7]); // G
            packet[i*3+1] = interleave(line0[i*9],   line1[i*9+3], line2[i*9+6]); // R
            packet[i*3+2] = interleave(line0[i*9+2], line1[i*9+5], line2[i*9+8]); // B
        }
    }
}


constexpr uint8_t blankline[720] = {};

const uint8_t * extractline(const image_info * info, const int32_t y){
    if(y < 0){
        return blankline;
    }
    
    const uint32_t limit = info->mirror ? info->height * 2 : info->height;
    if(!info->loop && y >= limit){
        return blankline;
    }

    const auto mody = y % limit;
    if(info->mirror && mody >= info->height){
        return &(info->image[3 * info->width * (limit - mody - 1)]);
    }

    return &(info->image[3 * info->width * mody]);
}


image_info * loadImage(){
    image_info * info;
    auto dip_state = get_dip_value();

    switch(dip_state & 0x0000000F){
        default:
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:  info = &info_bluewave;   break;
        case 9:  info = &info_symbol;     break;
        case 10: info = &info_rainbow;    break;
        case 11:
        case 12: info = &info_red;        break;
        case 13: info = &info_green;      break;
        case 14: info = &info_blue;       break;
        case 15: info = &info_blue;       break;
    }
    return info;
}


int main()
{
    sw_pins_init();
    usr_led_init();
    pio_init();

    auto info = loadImage();
    auto dip_state = get_dip_value();
    bool reverse = (dip_state & 0x00000010) == 0x00000010;

    gpio_put(USR_LED_PIN, reverse);

    // State transition
    // RUN  --(Draw finished && loop is disabled)-> HALT
    // RUN  --(Push SW is pressed)-> WAIT
    // HALT --(Push SW is pressed)-> WAIT
    // WAIT --(Push Sw is released)-> RUN

    uint32_t pio_packet[3*LENGTH];
    int32_t idx = 0;
    while(1){
        // State WAIT:
        // Suppress output while push switch is down
        if(psw_pressed){
            if(gpio_get(PSW_PIN)){
                psw_pressed = false;
                idx = info->multiline ? -2 : 0;
                info = loadImage();
                continue;
            }

            pack_parallel(pio_packet, blankline);
            dma_channel_set_read_addr(DMA0, (void*)pio_packet, true);
            sleep_us(POLL_GPIO_us);
            continue;
        }

        // State HALT:
        if(idx == INT32_MIN){
            sleep_us(POLL_GPIO_us);
            continue;
        }

        // State RUN:
        // Refresh LEDs periodically
        auto t = time_us_32();
        if(info->multiline){
            pack_parallel_sft(pio_packet, extractline(info, idx), extractline(info, idx+1), extractline(info, idx+2), reverse);
        }else{
            pack_parallel(pio_packet, extractline(info, idx));
        }
        dma_channel_set_read_addr(DMA0, (void*)pio_packet, true);

        const uint32_t limit = info->mirror ? info->height * 2 : info->height;
        if(++idx >= limit){
            // Switch to State HALT here
            idx = info->loop ? 0 : INT32_MIN;
        }
        sleep_us_since(info->period_us, t);
    }

}
