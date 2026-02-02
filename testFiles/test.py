import getopt, sys

args = sys.argv[1:]
options = "hmo:"
long_options = ["Help", "My_file", "Output="]

try:
    arguments, values = getopt.getopt(args, options, long_options)
    for currentArg, currentVal in arguments:
        if currentArg in ("-h", "--Help"):
            print("Showing Help")
        elif currentArg in ("-m", "--My_file"):
            print("File name:", sys.argv[0])
        elif currentArg in ("-o", "--Output"):
            print("Output mode:", currentVal)
except getopt.error as err:
    print(str(err))