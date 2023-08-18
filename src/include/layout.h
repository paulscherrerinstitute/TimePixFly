#ifndef LAYOUT_H
#define LAYOUT_H

constexpr static unsigned chip_size = 256;

struct chip_position final {
    unsigned x;
    unsigned y;
};

struct detector_layout final {
    unsigned width;
    unsigned height;
    std::vector<chip_position> chip;
};

#endif
