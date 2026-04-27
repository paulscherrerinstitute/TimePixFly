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

        "--reorder", "-r"
        metavar = "C,I,O"
        help = "reorder one chunk"
        arg_type = String
        help = "reorder (C)hip chunk (I)d by (O)ffset"
        required = false
        default = nothing
    end

    args = parse_args(settings)
    return args
end

function parse_reorder(spec::String)
    Tuple(parse.(UInt64, split(spec, ",")))
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

function reorder_chunk(events::AbstractArray{UInt64}, N::Int, cio::Tuple{UInt64, UInt64, UInt64})::Vector{UInt64}
    pkg_start = UInt64(0)
    pkg_end = UInt64(0)
    dest_end = UInt64(0)

    pkg_chip = cio[1]
    pkg_id = cio[2]
    dest_id = cio[2] + cio[3]

    i = UInt64(1)
    while i <= N
        chip, id, size = parse_header(events, i)

        if (chip == pkg_chip)
            if (id == pkg_id)
                pkg_start = i
                pkg_end = i + size
            elseif (id == dest_id)
                dest_end = i + size
                break
            end
        end

        i += size + 1
    end

    println("Reordering: [1:$pkg_start-1] [$pkg_end+1:$dest_end] [$pkg_start:$pkg_end] [$dest_end+1:end]")
    result = vcat([events[1:pkg_start-1], events[pkg_end+1:dest_end], events[pkg_start:pkg_end], events[dest_end+1:end]]...)
    return result
end

function check_sequence(events::AbstractArray{UInt64}, N::Int)
    ntoa = UInt64(0);
    ntdc = UInt64(0);

    pkg_ids = Dict{UInt64, UInt64}()
    pkg_sz = Dict{UInt64, Tuple{UInt64,UInt64}}()

    i = UInt64(1)
    while i <= N
        chip, id, size = parse_header(events, i)

        sz = get(pkg_sz, chip, (0, 0))
        pkg_sz[chip] = (sz[1]+1, sz[2]+size)
        expect = get(pkg_ids, chip, 0)
        if expect != id
            println("Bad id $id at offset $i: expected chunk id $expect instead of $id for chip $chip")
        end
        pkg_ids[chip] = id + 1
        i += size + 1
    end

    println("Sequence numbers:")
    for (k, v) in pairs(pkg_ids)
        println("chip $k -> $v")
    end

    println("Statistic:")
    for (k, v) in pairs(pkg_sz)
        avg_sz = string(div(round(v[2] / v[1], digits=1), sizeof(UInt)))
        println("chip $k -> ", v[1], " chunks, average size is $avg_sz")
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

    if (args["reorder"] !== nothing)
        cio = parse_reorder(args["reorder"])
        result = reorder_chunk(events, N, cio)
        events = nothing

        fname = "reordered-$fname"
        println("writing to reordered file $fname")
        open(fname, "w") do file
            write(file, result)
        end
    else
        check_sequence(events, N)
    end
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
