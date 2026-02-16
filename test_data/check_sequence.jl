using ArgParse

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

    ntoa = UInt64(0);
    ntdc = UInt64(0);

    id_mask = UInt64((1 << 48) - 1)
    header_mask = UInt64((1 << 32) - 1)
    header_id = UInt64(861425748);
    pkg_ids = Dict{UInt64, UInt64}()

    i = UInt64(1)
    while i <= N
        hid = events[i] & header_mask
        if hid != header_id
            throw(DomainError(hid, "offset $i: expected chunk header $header_id"))
        end
        chip = (events[i] >> 32) & 0xff
        size = events[i] >> 48
        if size % sizeof(UInt64) != 0
            throw(DomainError(size, "offset $i: bogus byte size for chunk $id"))
        end
        size = div(size, sizeof(UInt64))
        if size < 2
            throw(DomainError(size, "offset $i: chunk size too small"))
        end
        id = events[i+1] & id_mask
        expect = get(pkg_ids, chip, 0)
        if expect != id
            throw(DomainError(id, "offset $i: expected chunk id $expect instead of $id"))
        else
            pkg_ids[chip] = id + 1
        end
        i += size + 1
    end

    println("Sequence numbers:")
    for (k, v) in pairs(pkg_ids)
        println("chip $k -> $v")
    end
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
