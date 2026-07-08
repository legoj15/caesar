#include "Common.hpp"
#include "Csar.hpp"

#include <cstring>
#include <exception>
#include <iostream>
#include <string>

using namespace std;

int main(int argc, char* argv[])
{
	bool p = false;
	string outputDir;
	bool haveInput = false;

	if (argc == 1)
	{
		cout << "OVERVIEW: Caesar" << endl << endl;
		cout << "USAGE: caesar [options] <inputs>" << endl << endl;
		cout << "OPTIONS:" << endl;
		cout << "\t-p\t\tDo not ignore pan values of stereo samples" << endl;
		cout << "\t-w\t\tShow warnings" << endl;
		cout << "\t-o <dir>\tWrite output under <dir> (default: beside each input)" << endl;

		return 1;
	}

	int exitCode = 0;

	for (int i = 1; i < argc; ++i)
	{
		if (!strcmp(argv[i], "-p"))
		{
			p = true;
		}
		else if (!strcmp(argv[i], "-w"))
		{
			Common::ShowWarnings = true;
		}
		else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output-dir"))
		{
			if (i + 1 >= argc)
			{
				cerr << "ERROR\t\t" << argv[i] << " requires a directory argument" << endl;

				return 1;
			}

			outputDir = argv[++i];
		}
		else
		{
			haveInput = true;

			// Isolate each input: a bad file reports cleanly and the rest
			// still process, instead of aborting the whole run.
			try
			{
				Csar csar(argv[i], p);

				if (!csar.Extract(outputDir))
				{
					exitCode = 1;
				}
			}
			catch (const exception& e)
			{
				cerr << endl;
				cerr << "ERROR IN\t" << argv[i] << endl;
				cerr << "MESSAGE\t\t" << e.what() << endl;
				cerr << endl;

				Common::Reset();
				exitCode = 1;
			}
		}
	}

	if (!haveInput)
	{
		cerr << "ERROR\t\tno input archive given" << endl;

		return 1;
	}

	return exitCode;
}
