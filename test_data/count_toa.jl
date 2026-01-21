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
    N = length(data);

    if (mod(N, 8 ) != 0)
        error("file length not a multiple of 8 bytes")
    end

    ntoa = UInt64(0);
    ntdc = UInt64(0);

    for i in 8:8:N
        if data[i] == 0xb
            ntoa += 1;
        elseif data[i] == 0x6
            ntdc += 1;
        end
    end

    println("length $N\ntoa $ntoa\ntdc $ntdc")
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
