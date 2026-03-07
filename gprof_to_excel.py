import pandas as pd
import re
import argparse
import sys

def gprof_to_excel(input_file, output_file):
    with open(input_file, 'r') as f:
        lines = f.readlines()

    data = []
    row_regex = re.compile(r'^\s*(\d+\.\d+)\s+(\d+\.\d+)\s+(\d+\.\d+)\s+(\d+)?\s+(\d+\.\d+)?\s+(\d+\.\d+)?\s+(.*)$')


    for line in lines:
        match = row_regex.match(line)
        if match:
            data.append(match.groups())


    columns = [
        '% Time', 'Cumulative Seconds', 'Self Seconds', 'Calls', 'Self ms/call', 'Total ms/call', 'Function name'
    ]

    df = pd.DataFrame(data, columns=columns)

    for col in columns[:-1]:
        df[col] = pd.to_numeric(df[col], errors='coerce')

    df.to_excel(output_file, index=False)
    print(f"Success! Profile saved to {output_file}")


def main():
    parser = argparse.ArgumentParser(description="Convert gprof text output to an Excel spreadsheet")

    parser.add_argument("-i", "--input", required=True, help="Path to the input gprof text file (e.g. profile.txt")
    parser.add_argument("-o", "--output", required=True, help="Path to the output excel spreadsheet (default: gprof_analysis.xlsx)")

    args = parser.parse_args()
    gprof_to_excel(args.input, args.output)

if __name__ == "__main__":
    main()


