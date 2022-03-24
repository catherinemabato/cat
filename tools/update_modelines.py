#!/bin/python

import re
import sys
from enum import Enum

CLANG_OFF='// clang-format off\n'
NOTIFICATION_LINE='// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh\n'
VIM_MODELINE='// vim: shiftwidth=2 expandtab tabstop=2 cindent\n'
KATE_MODELINE='// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;\n'
CLANG_ON='// clang-format on\n'

class estate(Enum):
	none=1
	start=2
	inside=3
	end=4

state = estate.none
begin = -1
end = -1

lines = []

def process_line(line, lineno):
	global state
	global begin
	global end
	if(state == estate.none):
		if(line.startswith(CLANG_OFF)):
			state=estate.start
		return
	elif(state == estate.start):
		if(line.startswith('// modelines:')):
			state=estate.inside
			begin = lineno-1
		else:
			state=estate.none
		return
	elif(state == estate.inside):
		if(line.startswith(CLANG_ON)):
			state=estate.end
			end = lineno
		return
	elif(state == estate.end):
		if(not line.strip()):
			end = lineno
		else:
			state=estate.none
		return
		
def remove_lines():
	with open(sys.argv[1],'w') as file:
		for lineno,line in enumerate(lines):
			if((lineno >= begin) and (lineno <= end)):
				continue
			if(line.startswith('// modelines')):
				continue
			if(line.startswith('// vim')):
				continue
			if(line.startswith('// kate')):
				continue
			file.write(line)

if __name__ == "__main__":
	if(sys.argv[1] is None):
		raise RuntimeError("no input file")
	print('parsing {}'.format(sys.argv[1]));
	with open(sys.argv[1],'r') as file:
		lines = file.readlines()
	for lineno,line in enumerate(lines):
		process_line(line, lineno)
	if((begin == -1) != (end == -1)):
		raise RuntimeError("parsing error")
	print('removing old modelines');
	remove_lines()
	with open(sys.argv[1],'a') as file:
		print('adding new modelines');
		file.write(CLANG_OFF)
		file.write(NOTIFICATION_LINE)
		file.write(VIM_MODELINE)
		file.write(KATE_MODELINE)
		file.write(CLANG_ON)
		file.write('\n')

