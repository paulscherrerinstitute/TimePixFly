using ArgParse

function arg_parse()
    settings = ArgParseSettings(
        description = """Aggregate weights in XES output files."""
    )

    @add_arg_table settings begin
        "file"
            help = "input file(s)"
            arg_type = String
            required = true
            nargs='+'
    end

    args = parse_args(settings)
    return args
end

function main()
    args = arg_parse()
    files = args["file"]

    content = map(fname->parse.(Float32, split(String(read(fname)))), files)
    aggregated = reduce(+, content)

    println("Aggregated energy: ", sum(aggregated))
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
