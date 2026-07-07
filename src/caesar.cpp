#include "Common.hpp"
#include "Csar.hpp"

#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <system_error>

using namespace std;

int main(int argc, char* argv[])
{
	bool p = false;

	if (argc == 1)
	{
		cout << "OVERVIEW: Caesar" << endl << endl;
		cout << "USAGE: caesar [options] <inputs>" << endl << endl;
		cout << "OPTIONS:" << endl;
		cout << "\t-p\tDo not ignore pan values of stereo samples" << endl;
		cout << "\t-w\tShow warnings" << endl;

		return 1;
	}

	int exitCode = 0;

	// Extraction changes the working directory into each archive's output
	// folder, so remember where we started and return there between inputs.
	error_code ec;
	filesystem::path startDir = filesystem::current_path(ec);

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
		else
		{
			// Isolate each input: a bad file reports cleanly and the rest
			// still process, instead of aborting the whole run.
			try
			{
				Csar csar(argv[i], p);

				if (!csar.Extract())
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

			filesystem::current_path(startDir, ec);
		}
	}

	return exitCode;
}
