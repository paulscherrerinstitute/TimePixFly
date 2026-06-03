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

        "--plot", "-p"
        metavar = "CHIP"
        arg_type = Int64
        default = -1

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

function parse_tdcs(tdcs::Vector{Float64}, events::AbstractArray{UInt64})
    for ev in events
        if ev >> 60 == 0x6
            push!(tdcs, tdc_clock(ev))
        end
    end
end

function check_tdc_clock(events::AbstractArray{UInt64}, N::Int, plot::Int64, image_file::String)
    per_chip_tdcs = Dict{UInt64, Vector{Float64}}()

    i = UInt64(1)
    while i <= N
        chip, id, size = parse_header(events, i)

        if !haskey(per_chip_tdcs, chip)
            per_chip_tdcs[chip] = Vector{Float64}()
        end

        parse_tdcs(per_chip_tdcs[chip], view(events, i+2:i+size))
        i += size + 1
    end

    per_chip_tdc_diff = Dict{UInt64, Vector{Float64}}()
    for (chip, tdcs) in pairs(per_chip_tdcs)
        tdc_diffs = diff(tdcs)
        per_chip_tdc_diff[chip] = tdc_diffs
        if chip == plot
            fig = Figure(size = (600, 800))

            println("plotting tdc differences for chip $chip ...")
            ax1 = Axis(fig[1,1], title="TDC clock diffs", ylabel="tdc-diff")
            lines!(ax1, eachindex(tdc_diffs), tdc_diffs)

            println("plotting tdc timestamps for chip $chip ...")
            ax2 = Axis(fig[2,1], title="TDC clock", xlabel="period", ylabel="clock")
            lines!(ax2, eachindex(tdcs), tdcs)

            display(fig)

            if !isempty(image_file)
                save(image_file, fig)
            end
        end
    end

    println("Statistic:")
    for (chip, tdc_diffs) in pairs(per_chip_tdc_diff)
        s_len = length(per_chip_tdcs[chip])
        s_mean = mean(tdc_diffs)
        s_max = maximum(tdc_diffs)
        s_min = minimum(tdc_diffs)
        s_median = median(tdc_diffs)
        s_stddev = std(tdc_diffs)
        println("chip $chip -> $s_len tdcs, clock difference statistics: mean=$s_mean, median=$s_median, stddev=$s_stddev, min=$s_min, max=$s_max")

        if s_max > 2*s_median
            tdcs = per_chip_tdcs[chip]
            indices = findall(tdc_diffs .> 2*s_median)
            for i in indices
                println("   at $i: diff ", tdc_diffs[i], " between tdcs (", tdcs[i], ", ", tdcs[i+1], ")")
            end
        end
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

    plot = args["plot"]
    if plot >= 0
        WGLMakie.activate!()
    end
    image_file = args["save"]

    check_tdc_clock(events, N, plot, image_file)
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
