using ArgParse
using Sockets
import HTTP
import JSON

function arg_parse()
    settings = ArgParseSettings(
        description = """Save raw TCP event stream from serval to file."""
    )

    @add_arg_table settings begin
        "--server", "-s"
            metavar = "host:port"
            help = "server address"
            arg_type = String
            default = "localhost:8080"
        "--address", "-a"
            metavar = "host:port"
            help = "client address"
            arg_type = String
            default = "localhost:8081"
        "--file"
            metavar = "path"
            help = "where to store the raw data, print to stdout if empty"
            arg_type = String
            default = ""
    end

    args = parse_args(settings)

    (sn, sp) = rsplit(args["server"], ":"; limit=2)
    (cn, cp) = rsplit(args["address"], ":"; limit=2)
    fn::String = args["file"]

    saddr = getaddrinfo(sn)
    sport = parse(Int, sp)

    caddr = getaddrinfo(cn)
    cport = parse(Int, cp)

    return (saddr, sport, caddr, cport, fn)
end

function setup(saddr::IPAddr, sport::Int, caddr::IPAddr, cport::Int, fpath::String)
    shost = if typeof(saddr) == IPv6
        "[" * string(saddr) * "]"
    else
        string(saddr)
    end

    chost = if typeof(caddr)== IPv6
        "[" * string(caddr) * "]"
    else
        string(caddr)
    end

    json = JSON.json(Dict("Raw" => [ Dict("Base" => "tcp://connect@" * chost * ":" * string(cport)) ]))
    server = "http://" * shost * ":" * string(sport)

    println("configure connection address ", chost, ":", cport, " for server ", shost, ":", sport, "...")
    resp = HTTP.put(server * "/server/destination", ["Content-Type" => "application/json"], json)    
    println(resp.status, ": ", String(resp.body))

    println("listening to ", caddr, ":", cport, "...")
    client = listen(caddr, cport)

    println("open file ", fpath, " for writing...")
    file = if isempty(fpath)
        stdout
    else
        open(fpath, "w")
    end

    return (server, client, file)
end

function start(server, client, file)
    task = @async begin
        conn = accept(client)
        println("copying stream to file...")
        for d in readeach(conn, UInt8)
            write(file, d)
        end
    end

    println("start measurement...")
    resp = HTTP.get(server * "/measurement/start")
    println(resp.status, ": ", String(resp.body))

    println("wait for completion...")
    wait(task)
    println("OK")
end

function main()
    args = arg_parse()
    cons = setup(args...)
    start(cons...)
    # stop(cons...)
end

if abspath(PROGRAM_FILE) == @__FILE__
    main()
end
