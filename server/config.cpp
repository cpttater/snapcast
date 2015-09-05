/***
    This file is part of snapcast
    Copyright (C) 2015  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fstream>


using namespace std;


Config::Config()
{
	string dir = getenv("HOME") + string("/.config/SnapCast/");
	mkdir(dir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	filename_ = dir + "settings.json";
	cerr << filename_ << "\n";
//	fs::create_directory(filename_.parent_path());
}


void Config::save()
{
	std::ofstream ofs(filename_.c_str(), std::ofstream::out|std::ofstream::trunc);
	json clients = {
		{"Client", getClientInfos()}
	};
	ofs << std::setw(4) << clients;
	ofs.close();
}


void Config::test()
{
	ifstream ifs("snapserver.json", std::ifstream::in);
	json j;
	ifs >> j;
	std::cout << std::setw(4) << j << std::endl;
	json j1 = j["Client"];
	for (json::iterator it = j1.begin(); it != j1.end(); ++it)
	{
		ClientInfo client;
		client.fromJson(*it);
		std::cout << "Client:\n" << std::setw(4) << client.toJson() << '\n';
	}
}


ClientInfoPtr Config::getClientInfo(const std::string& macAddress, bool add)
{
	if (macAddress.empty())
		return nullptr;

	for (auto client: clients)
	{
		if (client->macAddress == macAddress)
			return client;
	}

	if (!add)
		return nullptr;

	ClientInfoPtr client = make_shared<ClientInfo>(macAddress);
	clients.push_back(client);

	return client;
}


json Config::getClientInfos() const
{
	json result = json::array();
	for (auto client: clients)
		result.push_back(client->toJson());
	return result;
}

