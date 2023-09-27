using ArgParse

function gen_ystripes(area, ystripes)
    dims = size(area)
    intervals = collect(zip(ystripes, ystripes[2:end]))
    for row in 1:dims[1]
        for stripe in pairs(intervals)
            i = stripe.first - 1
            from = stripe.second[1]
            to = stripe.second[2] - 1
            for col in from:to
                area[row, col] = i
            end
        end
    end
end

function write_map(io, area)
    dims = size(area)
    chips = dims .÷ 256
    chip = 0
    for chipx in 1:chips[1]
        for chipy in 1:chips[2]
            xh = chipx * 256
            xl = xh - 255
            yh = chipy * 256
            yl = yh - 255
            println("chip ", chip, ": x", xl, "-", xh, " y", yl, "-", yh)
            subarea = permutedims(area[xl:xh, yl:yh], (2,1))
            for p in eachindex(subarea)
                if subarea[p] !== nothing
                    println(io, chip, ",", p-1, ",", subarea[p], ",1.0")
                end
            end
            chip += 1
        end
    end
end

function arg_parse()
    settings = ArgParseSettings(
        description = """Generate pixel to XES energy point mapping file."""
    )

    @add_arg_table settings begin
        "--width", "-Y"
            metavar = "N"
            help = "width in number of pixels"
            arg_type = Int
            default = 512
        "--height", "-X"
            metavar = "N"
            help = "height in number of pixels"
            arg_type = Int
            default = 512
        "--ystripes", "-y"
            metavar = "start:step:end"
            help = "vertical stripes"
            arg_type = String
            default = ""
        "file_path"
            help = "where to store the mapping"
            arg_type = String
            default = ""
    end

    args = parse_args(settings)

    fname::String = args["file_path"]
    width::Int = args["width"]
    height::Int = args["height"]

    ystripes::Union{Nothing, StepRange{Int,Int}} = nothing
    if length(args["ystripes"]) > 0
        ystripes = StepRange{Int,Int}([parse(Int,d) for d in split(args["ystripes"], ":")]...)
    end

    return (width, height, ystripes, fname)
end

function main()
    (width, height, ystripes, fname) = arg_parse()
    area = Array{Union{Nothing, Int}}(nothing, height, width)
    gen_ystripes(area, ystripes)
    if length(fname) > 0
        println("writing to file ", fname, "...")
        open(fname, "w") do io
            write_map(io, area)
        end
    else
        write_map(stdout, area)
    end
    println("done.")
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
