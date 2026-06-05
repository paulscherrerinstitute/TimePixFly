using ArgParse
using Statistics
using WGLMakie
using AlgebraOfGraphics

function arg_parse()
    settings = ArgParseSettings(
        description = """Check the sequence of chunks is ordered."""
    )

    @add_arg_table settings begin
        "--file", "-f"
        metavar = "FNAME"
        arg_type = String
        help = "input file name"
        required = true

        "--begin", "-b"
        metavar = "BEGIN"
        arg_type = Int64
        help = "Ignore relative time below"
        default = 0

        "--step", "-t"
        metavar = "STEP"
        arg_type = Int64
        help = "Histogram time step"
        default = 1

        "--nbins", "-n"
        metavar = "NBINS"
        arg_type = Int64
        help = "Number of histogram bins"
        default = 1000

        "--save", "-s"
        metavar = "IMAGE"
        arg_type = String
        default = ""
    end

    args = parse_args(settings)
    return args
end

const id_mask = UInt64((1 << 48) - 1)
const header_mask = UInt64((1 << 32) - 1)
const header_id = UInt64(861425748);

function parse_header(events::AbstractArray{UInt64}, i::UInt64)::Tuple{UInt64, UInt64, UInt64}
    hid = events[i] & header_mask
    if hid != header_id
        throw(DomainError(hid, "offset $i: expected chunk header $header_id"))
    end
    id = events[i+1] & id_mask
    chip = (events[i] >> 32) & 0xff
    size = events[i] >> 48
    if size % sizeof(UInt64) != 0
        throw(DomainError(size, "offset $i: bogus byte size for chunk $id"))
    end
    size = div(size, sizeof(UInt64))
    if size < 2
        throw(DomainError(size, "offset $i: chunk size too small"))
    end
    return tuple(chip, id, size)
end

function tdc_clock(tdc::UInt64)::UInt64
    tdc_coarse = (tdc >> 9) & 0x7ffffffff
    fract = (tdc >> 5) & 0xf
    return (tdc_coarse << 1) | UInt64(fract > 6)
end

function toa_clock(toa::UInt64)::UInt64
    stamp = (toa & 0xffff) << 18
    toa >>= 16
    ftoa = toa & 0xf
    toa >>= 14
    stamp += (toa & 0x3fff) << 4
    return stamp - ftoa
end

ChipEvents = NamedTuple{(:tdcs, :toas), Tuple{Vector{UInt64}, Vector{UInt64}}}

function parse_events(chevs::ChipEvents, events::AbstractArray{UInt64})
    for ev in events
        t = ev >> 60
        if t == 0x6
            push!(chevs.tdcs, tdc_clock(ev))
        elseif t == 0xb
            push!(chevs.toas, toa_clock(ev))
        end
    end
end

function histogram(chevs::ChipEvents, start::Int64, step::Int64, nbins::Int64)::Vector{UInt64}
    tdcs = chevs.tdcs
    toas = chevs.toas
    histo = zeros(UInt64, nbins)

    i, j = 1, 1
    while j <= length(toas) && i < length(tdcs)
        toa = toas[j]
        tdc = tdcs[i]

        if toa >= tdcs[i+1]
            i += 1
            continue
        end

        j += 1
        if toa < tdc
            continue
        end

        relt = toa - tdc
        if relt >= start
            bin = div((toa - tdc) - start, step) + 1
            if bin <= nbins
                histo[bin] += 1
            end
        end
    end

    return histo
end

function check_histo(events::AbstractArray{UInt64}, N::Int, start::Int64, step::Int64, nbins::Int64, image_file::String)
    per_chip_events = Dict{UInt64, ChipEvents}()

    println("Collecting events ...")

    i = UInt64(1)
    while i <= N
        chip, id, size = parse_header(events, i)

        if !haskey(per_chip_events, chip)
            per_chip_events[chip] = (tdcs=[], toas=[])
        end

        parse_events(per_chip_events[chip], view(events, i+2:i+size))
        i += size + 1
    end

    bin_centers = start .+ (0:nbins-1) * step .+ (step * .5)
    fig = Figure()
    ncols = 4

    for (chip, chevs) in pairs(per_chip_events)
        println("chip $chip: sort events ...")
        sort!(chevs.tdcs)
        sort!(chevs.toas)

        begin
            println("chip $chip: histogramming ...")
            histo = histogram(chevs, start, step, nbins)

            row = Int64(div(chip, ncols) + 1)
            col = Int64(chip % ncols + 1)

            println("chip $chip: plotting histogram at $row, $col ...")
            ax = Axis(fig[row, col], title="chip $chip")
            barplot!(ax, bin_centers, histo; width=step*.9)
        end
    end

    display(fig)

    if !isempty(image_file)
        save(image_file, fig)
    end
end

function main()
    args = arg_parse()

    fname = args["file"]
    data = read(fname);
    if (mod(length(data), 8) != 0)
        throw(ArgumentError("file length not a multiple of 8 bytes"))
    end

    events = reinterpret(UInt64, data)
    data = nothing
    N = length(events)

    image_file = args["save"]
    start = args["begin"]
    step = args["step"]
    nbins = args["nbins"]

    check_histo(events, N, start, step, nbins, image_file)
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
