"""Plain Python baseline: parses an ibnetdiscover file into nodes and links and resolves peers."""
import re
import sys

HEADER = re.compile(r'^(Switch|Ca|Rt)\s+(\d+)\s+"([SHR])-([0-9a-fA-F]+)"\s*#\s*"([^"]*)"(.*)$')
LINK = re.compile(r'^\[(\d+)\](?:\(([0-9a-fA-F]+)\))?\s*"([SHR])-([0-9a-fA-F]+)"\[(\d+)\](?:\(([0-9a-fA-F]+)\))?\s*#\s*(.*)$')
SPEED = re.compile(r'(\d+)x([A-Z0-9]+)\s*$')


def parse(path):
    nodes = {}
    links = []
    cur = None
    with open(path) as fh:
        for line in fh:
            line = line.rstrip()
            if not line or line[0] == '#':
                continue
            if line[0] == '[':
                m = LINK.match(line)
                if not m or cur is None:
                    continue
                sp = SPEED.search(m.group(7))
                links.append((cur, int(m.group(1)), int(m.group(4), 16), int(m.group(5)),
                              sp.group(2) if sp else '?', int(sp.group(1)) if sp else 0))
                continue
            m = HEADER.match(line)
            if m:
                cur = int(m.group(4), 16)
                nodes[cur] = (m.group(1), m.group(5))
    return nodes, links


if __name__ == '__main__':
    n, l = parse(sys.argv[1])
    resolved = sum(1 for link in l if link[2] in n)
    print(len(n), len(l), resolved)
