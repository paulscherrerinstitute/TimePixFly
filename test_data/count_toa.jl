using ArgParse
using WGLMakie

function arg_parse()
    settings = ArgParseSettings(
        description = """Count TOA and TDC events."""
    )

    @add_arg_table settings begin
        "--file", "-f"
        metavar = "FNAME"
        arg_type = String
        help = "input file name"
        required = true

        "--tdc-errors", "-e"
        help = "plot TDC error time stamps"
        nargs=0

        "--image", "-i"
        arg_type = String
        help = "TDC error plot image file"
        default = ""
    end

    args = parse_args(settings)
    return args
end

function tdc_clock(tdc::UInt64)::UInt64
    tdc_coarse = (tdc >> 9) & 0x7ffffffff
    fract = (tdc >> 5) & 0xf
    return (tdc_coarse << 1) | UInt64(fract > 6)
end

function main()
    args = arg_parse()

    fname = args["file"]
    data = read(fname);
    if (mod(length(data), 8) != 0)
        error("file length not a multiple of 8 bytes")
    end

    events = reinterpret(UInt64, data)
    data = nothing
    N = length(events)

    ntoa = UInt64(0)
    ntdc = UInt64(0)
    nerr = UInt64(0)
    err_ts = Vector{UInt64}()

    for i in 1:N
        ev = events[i]
        t = ev >> 60
        if t == 0xb
            ntoa += 1
        elseif t == 0x6
            ntdc += 1
            if (ev >> 5) & 0xf == 0
                nerr += 1
                append!(err_ts, tdc_clock(ev))
            end
        end
    end

    nevents = ntoa + ntdc

    println("length $N\ntoa $ntoa\ntdc $ntdc ($nerr err)\nevents $nevents")

    if args["errors"]
        println("tdc errors: ", Float64.(err_ts))

        fig = Figure(size=(800, 150))

        println("plotting tdc errors ...")
        ax = Axis(fig[1,1], title="TDC fts errors", xlabel="time stamp")
        vlines!(ax, err_ts)

        xlims!(ax, minimum(err_ts) - 1, maximum(err_ts) + 1)
        ylims!(ax, .0, .1)
        hidespines!(ax, :l, :t, :r)
        ax.yticks = []
        autolimits!(ax)

        display(fig)

        image_file = args["image"]
        if !isempty(image_file)
            save(image_file, fig)
        end
    end
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
