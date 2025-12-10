/*!
\file
Provide pixel map parsing function
*/

#include <cassert>

#include "Poco/Exception.h"
#include "Poco/JSON/Object.h"
#include "Poco/JSON/Parser.h"
#include "Poco/JSON/PrintHandler.h"

#include "global.h"
#include "shared_types.h"
#include "json_ops.h"

namespace {

    /*!
    \brief Parse object of type T in a string
    \param s   String having a representation of T at pos
    \param pos Position in the string from where to parse
    \return Parsed object of type Tualified-id before ‘>’ token
    */
    template <typename T>
    T parse(std::string_view& s, std::string_view::size_type pos)
    {
            std::istringstream iss(s.substr(pos).data());
            T t;
            iss >> t;
            if (!iss)
                    throw std::ios_base::failure("failed to parse XESPoints file data");
            return t;
    }

    /*!
    \brief Parse mapping from JSON stream
    \verbatim
    {
        "type": "PixelMap",                 // optional
        "chips": [                          // per chip mapping
            [                               // chip 0: pixel mapping
                {                           // chip 0, flat pixel 0: mapping
                    "i":0,                  // flat pixel index
                    "p":[0,1,2],            // energy points
                    "f":[0.33,0.33,0.33]    // energy fractions
                },
                ...                         // chip 0: other pixels
            ],
            ...                             // other chips
        ]
    }
    \endverbatim
    \param pmap result mapping
    \param in JSON input stream
    */
    void from_json(PixelIndexToEp& pmap, std::istream& in)
    {
        const detector_layout& layout = global::instance->layout;
        const auto numPixels = chip_size * chip_size;
        const auto numChips = layout.chip.size();

        pmap.chip.resize(numChips);
        for (auto& chip: pmap.chip) {
            chip.flat_pixel.resize(numPixels);
        }

        Poco::JSON::Parser parser;
        Poco::JSON::Object::Ptr json = parser
            .parse(in)
            .extract<Poco::JSON::Object::Ptr>();
        Poco::JSON::Array::Ptr chipList = json / "chips";
        using idx_t = std::remove_cv_t<decltype(chipList->size())>;
        auto nchips = chipList->size();
        if (numChips != nchips)
            throw Poco::RuntimeException{"mismatch with number of chips from detector server"};
        for (idx_t i=0; i<nchips; i++) {
            Poco::JSON::Array::Ptr pixelList = chipList / i;
            auto npixels = pixelList->size();
            for (idx_t j=0; j<npixels; j++) {
                Poco::JSON::Object::Ptr partLists = pixelList | j;
                unsigned index = partLists->getValue<unsigned>("i");
                if (index >= numPixels)
                    throw Poco::RuntimeException{"invalid pixel index"};
                Poco::JSON::Array::Ptr pointList = partLists / "p";
                Poco::JSON::Array::Ptr fractionList = partLists / "f";
                auto nparts = pointList->size();
                if (nparts != fractionList->size())
                    throw Poco::RuntimeException{"point/fraction list size mismatch"};
                pmap.chip[i].flat_pixel[index].part.resize(nparts);
                for (idx_t k=0; k<nparts; k++) {
                    auto p = pointList->getElement<unsigned>(k);
                    pmap.npoints = std::max(p, pmap.npoints);
                    auto f = fractionList->getElement<float>(k);
                    pmap.chip[i].flat_pixel[index].part[k] = {p, f};
                }
            }
        }

        pmap.npoints += 1;
    }

    /*!
    \brief Read region of interest related to area (pixel to energy point mapping)

    The file contains lines in the form

    chip flatPixel energyPoint0 weight0 [energyPoint1 weight1 ...]

    \param pmap Set this mapping to what was defined in XESPointsFile
    \param in file input stream
    */
    void from_file(PixelIndexToEp& pmap, std::istream& in)
    {
        const detector_layout& layout = global::instance->layout;
        const auto numPixels = chip_size * chip_size;
        const auto numChips = layout.chip.size();

        pmap.chip.resize(numChips);
        for (auto& chip: pmap.chip) {
            chip.flat_pixel.resize(numPixels);
        }

        constexpr size_t bufSize = 1024;
        char buf[bufSize] = {0};
        std::string_view::size_type posN[bufSize] = {0};
        if (! in)
            throw std::ios_base::failure{"bad XESPoints file input stream"};

        for (unsigned line=1;;line++) {
            // i, j, XESEnergyIndex[i,j,k]..., XESWeight [i,j,k]...
            if (! in.getline(buf, bufSize).good()) {
                if (! in.eof())
                    throw std::ios_base::failure(std::string{"failed to parse XESPoints file at line "} + std::to_string(line));
                break;
            }
            std::string_view s(buf);
            std::string_view::size_type pos = 0;
            unsigned count = 0;
            posN[count] = pos;
            while ((pos = s.find(',', pos)) != std::string_view::npos) {
                count++;
                pos++;
                posN[count] = pos;
            }
            count += 1;
            if (count < 2)
                throw std::invalid_argument("invalid XESPoints file line (count < 2)");
            if ((count % 2) != 0)
                throw std::invalid_argument("invalid XESPoints file line (count % 2 != 0)");
            unsigned k = parse<unsigned>(s, posN[0]);       // chip
            if (k >= numChips)
                throw std::invalid_argument("invalid chip number in XESPoints file");
            unsigned l = parse<unsigned>(s, posN[1]);       // flatPixel
            if (l >= numPixels)
                throw std::invalid_argument("invalid pixel number in XESPoints file");
            FlatPixelToEp& pixel = pmap.at(PixelIndex::from(k, l));
            const unsigned numEnergyPoints = (count - 2u) / 2u;
            for (unsigned m=0; m<numEnergyPoints; m++) {
                EpPart part{};
                part.energy_point = parse<unsigned>(s, posN[2+m]);
                pmap.npoints = std::max(pmap.npoints, part.energy_point);
                pixel.part.push_back(std::move(part));
            }
            for (unsigned m=0; m<numEnergyPoints; m++)
                pixel.part[m].weight = parse<float>(s, posN[2+numEnergyPoints+m]);
        }

        pmap.npoints += 1;
    }

}

void PixelIndexToEp::from(PixelIndexToEp& pmap, std::istream& in, unsigned type)
{
    switch (type) {
        case PixelIndexToEp::FILE_STREAM:
            from_file(pmap, in); break;
        case PixelIndexToEp::JSON_STREAM:
            from_json(pmap, in); break;
        default:
            throw Poco::LogicException{std::string{"Illegal pixelmap parsing type - "} + std::to_string(type)};
    };
}

std::ostream& operator<<(std::ostream& out, const PixelIndexToEp& pmap)
{
    Poco::JSON::PrintHandler json{out};
    json.startObject();
    json.key("type"); json.value(std::string{"PixelMap"});
    json.key("chips"); json.startArray();
    for (const auto& chip : pmap.chip) {
        json.startArray();
        const auto& flat = chip.flat_pixel;
        for (unsigned i=0; i<flat.size(); i++) {
            const auto& parts = flat[i].part;
            if (! parts.empty()) {
                json.startObject();
                json.key("i"); json.value(i);
                json.key("p"); json.startArray();
                std::for_each(std::cbegin(parts), std::cend(parts), [&](const auto& part) {
                    json.value(part.energy_point);
                });
                json.endArray();
                json.key("f"); json.startArray();
                std::for_each(std::cbegin(parts), std::cend(parts), [&](const auto& part) {
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
