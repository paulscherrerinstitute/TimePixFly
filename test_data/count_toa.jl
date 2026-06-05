using ArgParse

function arg_parse()
    settings = ArgParseSettings(
        description = """Check theres only one 'End of (sequential|data driven) readout'."""
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

    for i in 1:N
        ev = events[i]
        t = ev >> 60
        if t == 0xb
            ntoa += 1
        elseif t == 0x6
            ntdc += 1
            if (ev >> 5) & 0xf == 0
                nerr += 1
            end
        end
    end

    nevents = ntoa + ntdc

    println("length $N\ntoa $ntoa\ntdc $ntdc ($nerr err)\nevents $nevents")
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
