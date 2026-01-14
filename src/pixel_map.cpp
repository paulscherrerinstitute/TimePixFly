/*!
\file
Implement the output operator for PixelMap
*/

#include "Poco/JSON/PrintHandler.h"

#include "pixel_map.h"

std::ostream& operator<<(std::ostream& out, const PixelMap& pmap)
{
    const unsigned nchips = pmap.indices.size() / pmap.pixels_per_chip;
    Poco::JSON::PrintHandler json{out};
    json.startObject();
    json.key("type"); json.value(std::string{"PixelMap"});
    json.key("chips"); json.startArray();
    for (unsigned chip=0; chip<nchips; chip++) {
        json.startArray();
        for (unsigned i=0; i<pmap.pixels_per_chip; i++) {
            auto map_range = pmap[{chip, i}];
            if (!map_range.empty()) {
                json.startObject();
                json.key("i"); json.value(i);
                json.key("p"); json.startArray();
                std::for_each(std::cbegin(map_range), std::cend(map_range), [&](const auto& part) {
                    json.value(part.energy_point);
                });
                json.endArray();
                json.key("f"); json.startArray();
                std::for_each(std::cbegin(map_range), std::cend(map_range), [&](const auto& part) {
                    json.value(part.weight);
                });
                json.endArray();
                json.endObject();
            }
        }
        json.endArray();
    }
    json.endArray();
    json.endObject();
    return out;
}
