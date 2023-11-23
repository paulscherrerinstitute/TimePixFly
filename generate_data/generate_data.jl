using Base.Iterators
using ArgParse

"""
    hex(x)

Return hexadecimal representation of `x` as a `String`.
"""
function hex(x::UInt64)::String
    return string(x, base=16, pad=16)
end

"""
    get_bits(data, h, l)

Return bits between high bit `h` inclusive and low bit `l`.
"""
function get_bits(data::UInt64, h::Integer, l::Integer)::UInt64
    nbits = (h - l) + 1;
    mask::UInt64 = (1 << nbits) - 1;
    return (data >> l) & mask;
end

"""
    tdc_clock(data)

Extract TDC time in clock ticks from raw event `data`.
"""
function tdc_clock(tdc::UInt64)::Int64
    coarse = get_bits(tdc, 43, 9)
    fract = get_bits(tdc, 8, 5)
    @assert (1 <= fract) && (fract <= 12)
    return (coarse << 1) | div((fract - 1), 6)
end

"""
    toa_clock(data)

Extract TOA time in clock ticks from raw event `data`.
"""
function toa_clock(data::UInt64)::Int64
    ftoa = get_bits(data, 19, 16);
    toa = get_bits(data, 43, 30);
    coarse = get_bits(data, 15, 0);
    return (((coarse << 14) + toa) << 4) - ftoa;
end

"""
    tpx3()

Generate raw representation of the chunk header identification.
"""
function tpx3()::UInt64
    id = "3XPT"
    res::UInt64 = 0
    for c in id
        res = (res << 8) + UInt64(c)
    end
    return res
end

"""
    chunk_header(nbytes, chip)

Generate raw chunk header for `chip` containing `nbytes` payload.
"""
function chunk_header(nbytes::Integer, chip::Integer)::UInt64
    return (UInt64(nbytes) << 48) + (UInt64(chip) << 32) + tpx3()
end

"""
    toa_time(t)

Split clock tick `t` into TOA time representation parts.

Return array `[toa, ftoa, spidr]`.
"""
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

"""
    toa(pixaddr, toa, tot, ftoa, spidr)

Generate raw TOA event representation at time `toa` with fraction `ftoa`, `tot`, `spidr`,
and raw pixel coordinate representation `pixaddr`.
"""
function toa(pixaddr::Integer, toa::Integer, tot::Integer, ftoa::Integer, spidr::Integer)::UInt64
    return ((UInt64(0xb) << 60) + (UInt64(pixaddr) << 44)
          + (UInt64(toa) << 30) + (UInt64(tot) << 20)
          + (UInt64(ftoa) << 16) + (UInt64(spidr)))
end

"""
    toa(pixaddr, clk, tot)

Generate raw TOA event representation at time `clk` with `tot` and raw pixel coordinate
representation `pixaddr`.
"""
function toa(pixaddr::Integer, clk::Integer, tot::Integer)::UInt64
    t = toa_time(clk)
    return toa(pixaddr, t[1], tot, t[2], t[3])
end

"""
    tdc_time(t)

Return array `[time, tim efraction]` for time `t`.
"""
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

"""
    tdc(t, ft)

Return raw representation of TDC event at coarse time `t` with time fraction `ft`.
"""
function tdc(t::Integer, ft::Integer)::UInt64
    return (UInt64(0x6b) << 56) + (UInt64(t) << 9) + (UInt64(ft) << 5)
end

"""
    tdc(clk)

Return raw representation of TDC event at time `clk`.
"""
function tdc(clk::Integer)::UInt64
    t = tdc_time(clk)
    return tdc(t[1], t[2])
end

"""
    pkcount(count)

Return the raw representation packet ID `count`.
"""
function pkcount(count::Integer)::UInt64
    return (UInt64(0x50) << 56) + count
end

"""
    header(nevents, chip, count)

Generate raw event packet header for a packet with ID `count`, originating from `chip`
and containing `nevents` events.

Return `UInt64` vector representing the raw packet header.
"""
function header(nevents::Integer, chip::Integer, count::Integer)::Vector{UInt64}
    return [chunk_header(8 + 8 * nevents, chip), pkcount(count)]
end

"""
Raw event packet representation
"""
struct EventPacket
    "Chip that generated the events in this packet"
    chip::UInt32            # chip number
    "Packet ID"
    id::UInt32              # packet number
    "Start time in clock ticks"
    tstart::UInt64          # start clock counter
    "End time in clock ticks"
    tend::UInt64            # end clock counter
    "Number of TOA events in this packet"
    ntoa::UInt32            # number of TOA events
    "Number of TDC events in this packet - ignored for now"
    ntdc::UInt32            # number of TDC events, must be 1 currently
    "List of raw events"
    events::Vector{UInt64}  # list of raw events
end

"""
    generate_packet(chip, period_id, period, tot, ntoa, ntdc)

Generate an events packet for `chip`, for period `period_id`, with `ntoa` TOA events evenly spread out accross the interval `period`.
Each event contains `tot`. Generate one TDC event at the end of the period. `ntdc` must be 1 currently.
"""
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

"""
    write_packets(io, packets)

Write `packets` content in binary form to `io`.
"""
function write_packets(io, packets)
    for pk in packets
        write(io, header(length(pk.events), pk.chip, pk.id))
        write(io, pk.events)
    end
end

"""
    print_packets(io, packets)

Print `packets` content in readable form to `ìo`.
"""
function print_packets(io, packets)
    for pk in packets
        print((Int32(pk.chip), Int32(pk.id)), " ")
        for ev in flatten((header(length(pk.events), pk.chip, pk.id), pk.events))
            print(hex(ev), " ")
        end
    end
    println()
end

"""
    arg_parse()

Parse commandline arguments.

Return the tuple `(nchips, period, tot, tstart, tend, fname)``.
"""
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

"""
    main()

Parse commandline arguments, generate raw event data packets and either

- write raw event data packets to file `fname`
- or print them in human readable form to `stdout
"""
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
    println("nchips=", nchips, ", period=", period, ", nperiods=", npackets, ", tot=", tot, " - done.")
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
