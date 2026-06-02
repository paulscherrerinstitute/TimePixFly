using ArgParse
using Statistics

function arg_parse()
    settings = ArgParseSettings(
        description = """Check the sequence of chunks is ordered."""
    )

    @add_arg_table settings begin
        "--file", "-f"
        metavar = "FNAME"
        help = "input file"
        arg_type = String
        help = "input file name"
        required = true
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
    return (tdc_coarse << 1) | div(fract - 1, 6)
end

function parse_tdcs(tdcs::Vector{Float64}, events::AbstractArray{UInt64})
    for ev in events
        if ev >> 60 == 0x6
            push!(tdcs, tdc_clock(ev))
        end
    end
end

function check_tdc_clock(events::AbstractArray{UInt64}, N::Int)
    per_chip__tdcs = Dict{UInt64, Vector{Float64}}()

    i = UInt64(1)
    while i <= N
        chip, id, size = parse_header(events, i)

        if !haskey(per_chip__tdcs, chip)
            per_chip__tdcs[chip] = Vector{Float64}()
        end

        parse_tdcs(per_chip__tdcs[chip], view(events, i+2:i+size))
        i += size + 1
    end

    per_chip_tdc_diff = Dict{UInt64, Vector{Float64}}()
    for (chip, tdcs) in pairs(per_chip__tdcs)
        sort!(tdcs)
        per_chip_tdc_diff[chip] = diff(tdcs)
    end

    println("Statistic:")
    for (chip, tdcs_diffs) in pairs(per_chip_tdc_diff)
        s_len = length(per_chip__tdcs[chip])
        s_mean = mean(tdcs_diffs)
        s_max = maximum(tdcs_diffs)
        s_min = minimum(tdcs_diffs)
        s_median = median(tdcs_diffs)
        s_stddev = std(tdcs_diffs)
        println("chip $chip -> $s_len tdcs, clock difference statistics: mean=$s_mean, median=$s_median, stddev=$s_stddev, min=$s_min, max=$s_max")
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

    check_tdc_clock(events, N)
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
