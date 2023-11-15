#ifndef ENERGY_POINTS_H
#define ENERGY_POINTS_H

struct EpPart final {
        unsigned energy_point;                  //!< pixel contributes to this energy point
        float weight;                           //!< with this weight
};

struct FlatPixelToEp final {
        std::vector<EpPart> part;               //!< one part per energy point
};

struct ChipToEp final {
        std::vector<FlatPixelToEp> flat_pixel;
};

struct PixelIndexToEp final {
        std::vector<ChipToEp> chip;
        unsigned npoints = 0;

        inline FlatPixelToEp& operator[](const PixelIndex& index)
        {
                assert(index.chip < chip.size());
                assert(index.flat_pixel < chip[index.chip].flat_pixel.size());
                return chip[index.chip].flat_pixel[index.flat_pixel];
        }

        inline const FlatPixelToEp& operator[](const PixelIndex& index) const
        {
                assert(index.chip < chip.size());
                assert(index.flat_pixel < chip[index.chip].flat_pixel.size());
                return chip[index.chip].flat_pixel[index.flat_pixel];
        }

        inline FlatPixelToEp& at(const PixelIndex& index)
        {
                return chip.at(index.chip).flat_pixel.at(index.flat_pixel);
        }
};

#endif // ENERGY_POINTS_H
