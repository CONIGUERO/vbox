## @file
#
# Convert an AML file to a .hex file containing the AML bytecode stored in a
# C array.
# By default, "Tables\Dsdt.aml" will generate "Tables\Dsdt.hex".
# "Tables\Dsdt.hex" will contain a C array named "dsdt_aml_code" that contains
# the AML bytecode.
#
# Copyright (c) 2020, ARM Limited. All rights reserved.<BR>
#
# SPDX-License-Identifier: BSD-2-Clause-Patent
#

import argparse
import Common.EdkLogger as EdkLogger
from Common.BuildToolError import *
import sys
import os

## Parse the command line arguments.
#
# @retval A argparse.NameSpace instance, containing parsed values.
#
def ParseArgs():
    # Initialize the parser.
    Parser = argparse.ArgumentParser(
      description="Convert an AML file to a .hex file containing the AML " + \
                  "bytecode stored in a C array. By default, " + \
                  "\"Tables\\Dsdt.aml\" will generate" + \
                  "\"Tables\\Dsdt.hex\". \"Tables\\Dsdt.hex\" will " + \
                  "contain a C array named \"dsdt_aml_code\" that " + \
                  "contains the AML bytecode."
      )

    # Define the possible arguments.
    Parser.add_argument(
      dest="InputFile",
      help="Path to an input AML file to generate a .hex file from."
      )
    Parser.add_argument(
      "-o", "--out-dir", dest="OutDir",
      help="Output directory where the .hex file will be generated. " + \
           "Default is the input file's directory."
      )

    # Parse the input arguments.
    Args = Parser.parse_args()
    SplitInputName = ""

    if not os.path.exists(Args.InputFile):
        EdkLogger.error(__file__, FILE_OPEN_FAILURE,
                        ExtraData=Args.InputFile)
        return None
    else:
        with open(Args.InputFile, "rb") as fIn:
            Signature = str(fIn.read(4))
            if ("DSDT" not in Signature) and ("SSDT" not in Signature):
                EdkLogger.info("Invalid file type. " + \
                                "File does not have a valid " + \
                                "DSDT or SSDT signature: %s" % Args.InputFile)
                return None

    # Get the basename of the input file.
    SplitInputName = os.path.splitext(Args.InputFile)
    BaseName = os.path.basename(SplitInputName[0])

    # If no output directory is specified, output to the input directory.
    if not Args.OutDir:
        Args.OutputFile = os.path.join(
          os.path.dirname(Args.InputFile),
          BaseName + ".hex"
          )
    else:
        if not os.path.exists(Args.OutDir):
            os.mkdir(Args.OutDir)
        Args.OutputFile = os.path.join(Args.OutDir, BaseName + ".hex")

    Args.BaseName = BaseName

    return Args

## Convert an AML file to a .hex file containing the AML bytecode stored
#  in a C array.
#
# @param  InputFile     Path to the input AML file.
# @param  OutputFile    Path to the output .hex file to generate.
# @param  BaseName      Base name of the input file.
#                       This is also the name of the generated .hex file.
#
def AmlToHex(InputFile, OutputFile, BaseName):

    MacroName = "__{}_HEX__".format(BaseName.upper())
    ArrayName = BaseName.lower() + "_aml_code"

    with open(InputFile, "rb") as fIn, open(OutputFile, "w") as fOut:
        # Write header.
        fOut.write("// This file has been generated from:\n" + \
                   "// \tPython script: " + \
                   os.path.abspath(__file__) + "\n" + \
                   "// \tInput AML file: " + \
                   os.path.abspath(InputFile) + "\n\n" + \
                   "#ifndef {}\n".format(MacroName) + \
                   "#define {}\n\n".format(MacroName)
                   )

        # Write the array and its content.
        fOut.write("unsigned char {}[] = {{\n  ".format(ArrayName))
        cnt = 0
        byte = fIn.read(1)
        while len(byte) != 0:
            fOut.write("0x{0:02X}, ".format(ord(byte)))
            cnt += 1
            if (cnt % 8) == 0:
                fOut.write("\n  ")
            byte = fIn.read(1)
        fOut.write("\n};\n")

        # Write footer.
        fOut.write("#endif // {}\n".format(MacroName))

## Main method
#
# This method:
#   1-  Initialize an EdkLogger instance.
#   2-  Parses the input arguments.
#   3-  Converts an AML file to a .hex file containing the AML bytecode stored
#       in a C array.
#
# @retval 0     Success.
# @retval 1     Error.
#
def Main():
    # Initialize an EdkLogger instance.
    EdkLogger.Initialize()

    try:
        # Parse the input arguments.
        CommandArguments = ParseArgs()
        if not CommandArguments:
            return 1

        # Convert an AML file to a .hex file containing the AML bytecode stored
        # in a C array.
        AmlToHex(CommandArguments.InputFile, CommandArguments.OutputFile,
                          CommandArguments.BaseName)
    except Exception as e:
        print(e)
        return 1

    return 0

if __name__ == '__main__':
    r = Main()
    # 0-127 is a safe return range, and 1 is a standard default error
    if r < 0 or r > 127: r = 1
    sys.exit(r)
