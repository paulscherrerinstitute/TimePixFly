using Base.Iterators
using ArgParse

function hex(x::UInt64)::String
    return string(x, base=16, pad=16)
end

function get_bits(data::UInt64, h::Integer, l::Integer)::UInt64
    nbits = (h - l) + 1;
    mask::UInt64 = (1 << nbits) - 1;
    return (data >> l) & mask;
end

function tdc_clock(tdc::UInt64)::Int64
    coarse = get_bits(tdc, 43, 9)
    fract = get_bits(tdc, 8, 5)
    @assert (1 <= fract) && (fract <= 12)
    return (coarse << 1) | div((fract - 1), 6)
end

function toa_clock(data::UInt64)::Int64
    ftoa = get_bits(data, 19, 16);
    toa = get_bits(data, 43, 30);
    coarse = get_bits(data, 15, 0);
    return (((coarse << 14) + toa) << 4) - ftoa;
end

function tpx3()::UInt64
    id = "3XPT"
    res::UInt64 = 0
    for c in id
        res = (res << 8) + UInt64(c)
    end
    return res
end

function chunk_header(nbytes::Integer, chip::Integer)::UInt64
    return (UInt64(nbytes) << 48) + (UInt64(chip) << 32) + tpx3()
end

function toa_time(t::Integer)::Vector{UInt64}
    ticks = Int64(t)
    spidr::UInt64 = ticks >> 18
    ticks -= spidr << 18
    toa::UInt64 = ticks >> 4
    ticks -= toa << 4
    ftoa::UInt64 = 0
    if ticks > 0
        ftoa = 16 - ticks
        toa += 1
    end
    spidr &= (1 << 16) - 1
    return [toa, ftoa, spidr]
end

function toa(pixaddr::Integer, toa::Integer, tot::Integer, ftoa::Integer, spidr::Integer)::UInt64
    return ((UInt64(0xb) << 60) + (UInt64(pixaddr) << 44)
          + (UInt64(toa) << 30) + (UInt64(tot) << 20)
          + (UInt64(ftoa) << 16) + (UInt64(spidr)))
end

function toa(pixaddr::Integer, clk::Integer, tot::Integer)::UInt64
    t = toa_time(clk)
    return toa(pixaddr, t[1], tot, t[2], t[3])
end

function tdc_time(t::Integer)::Vector{UInt64}
    ticks = UInt64(t)
    coarse::UInt64 = ticks >> 1
    ticks -= coarse << 1
    fract::UInt64 = ticks * 6 + 1
    if fract > 12
        fract = 12
    end
    coarse &= (1 << 35) - 1
    return [coarse, fract]
end

function tdc(t::Integer, ft::Integer)::UInt64
    return (UInt64(0x6b) << 56) + (UInt64(t) << 9) + (UInt64(ft) << 5)
end

function tdc(clk::Integer)::UInt64
    t = tdc_time(clk)
    return tdc(t[1], t[2])
end

function pkcount(count::Integer)::UInt64
    return (UInt64(0x50) << 56) + count
end

function header(nevents::Integer, chip::Integer, count::Integer)::Vector{UInt64}
    return [chunk_header(8 + 8 * nevents, chip), pkcount(count)]
end

struct EventPacket
    chip::UInt32
    id::UInt32
    tstart::UInt64
    tend::UInt64
    ntoa::UInt32
    ntdc::UInt32
    events::Vector{UInt64}
end

function generate_packet(chip::Integer, period_id::Integer, period::Integer, tot::Integer, ntoa::Integer, ntdc::Integer)::EventPacket
    t = period_id * period
    dtdc = div(period, ntdc)
    events::Array{UInt64} = [ tdc(t) ]

    dtoa = div(period, ntoa)
    for i in 1:ntoa
        push!(events, toa(0, t + i * dtoa, tot))
    end

    if ntdc > 1
        push!(events, tdc(t + period))
    end

    return EventPacket(
        chip,
        period_id,
        period_id * period,
        (period_id + 1) * period,
        ntoa,
        ntdc,
        events
    )
end

function write_packets(io, packets)
    for pk in packets
        write(io, header(length(pk.events), pk.chip, pk.id))
        write(io, pk.events)
    end
end

function print_packets(io, packets)
    for pk in packets
        print((Int32(pk.chip), Int32(pk.id)), " ")
        for ev in flatten((header(length(pk.events), pk.chip, pk.id), pk.events))
            print(hex(ev), " ")
        end
    end
    println()
end

function arg_parse()
    settings = ArgParseSettings(
        description = """Generate a raw TPX3 file with contents as delivered by serval 3.20,
                         or print out the generated stream in hex."""
    )

    @add_arg_table settings begin
        "--nchips", "-c"
            metavar = "N"
            help = "number of chips"
            arg_type = Int
            default = 4
        "--period", "-p"
            metavar = "N"
            help = "period in ns"
            arg_type = Int
            default = 1000
        "--nperiods", "-n"
            metavar = "N"
            help = "number of periods"
            arg_type = Int
            default = 6
        "--tot", "-t"
            metavar = "N"
            help = "TOT value for TOA events"
            arg_type = Int
            default = 50
        "file_path"
            help = "where to store the raw data"
            arg_type = String
            default = ""
    end

    args = parse_args(settings)

    fname::String = args["file_path"]
    nchips::Int = args["nchips"]
    period::Int = args["period"]
    nperiods::Int = args["nperiods"]
    tot::Int = args["tot"]
    tstart = 0
    tend = nperiods * period
    return (nchips, period, tot, tstart, tend, fname)
end

function main()
    (nchips, period, tot, tstart, tend, fname) = arg_parse()
    npackets = round(UInt32, (tend - tstart) / period)

    packets = [ generate_packet(c, p, period, tot, 10, 1) for p in 0:npackets-1 for c in 0:nchips-1 ]

    if length(fname) > 0
        println("writing to raw file ", fname, "...")
        open(fname, "w") do io
            write_packets(io, packets)
        end
    else
        print_packets(stdout, packets)
    end
    println("(nchips=", nchips, ", period=", period, ", nperiods=", npackets, ", tot=", tot, " - done.")
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
