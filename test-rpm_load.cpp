/*
 * Copyright (C) 2013 Red Hat, Inc.
 * Written by Florian Weimer <fweimer@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "rpm_load.hpp"
#include "rpm_package_info.hpp"

#include "database.hpp"
#include "database_elf_closure.hpp"
#include "dir_handle.hpp"
#include "pg_testdb.hpp"
#include "pgconn_handle.hpp"
#include "pgresult_handle.hpp"
#include "string_support.hpp"
#include "symboldb_options.hpp"

#include "test.hpp"

static void
test()
{
  static const char DBNAME[] = "template1";
  pg_testdb testdb;
  {
    // Run this directly, to suppress notices.
    pgconn_handle db(testdb.connect(DBNAME));
    pgresult_handle res(PQexec(db.raw, database::SCHEMA));
  }

  symboldb_options opt;
  opt.output = symboldb_options::quiet;
  database db(testdb.directory().c_str(), DBNAME);

  {
    database::package_id last_pkg_id(0);
   static const char RPMDIR[] = "test/data";
    std::string rpmdir_prefix(RPMDIR);
    rpmdir_prefix += '/';
    dir_handle rpmdir(RPMDIR);
    while (dirent *e = rpmdir.readdir()) {
      if (ends_with(std::string(e->d_name), ".rpm")
	  && !ends_with(std::string(e->d_name), ".src.rpm")) {
	rpm_package_info info;
	database::package_id pkg
	  (rpm_load(opt, db, (rpmdir_prefix + e->d_name).c_str(), info));
	CHECK(pkg > last_pkg_id);
	last_pkg_id = pkg;
	pkg = rpm_load(opt, db, (rpmdir_prefix + e->d_name).c_str(), info);
	CHECK(pkg == last_pkg_id);
      }
    }
  }

  {
    static const char *const sysvinit_files_6[] = {
      "/sbin/killall5",
      "/sbin/pidof",
      "/sbin/sulogin",
      "/usr/bin/last",
      "/usr/bin/lastb",
      "/usr/bin/mesg",
      "/usr/bin/utmpdump",
      "/usr/bin/wall",
      "/usr/share/doc/sysvinit-tools-2.88",
      "/usr/share/doc/sysvinit-tools-2.88/Changelog",
      "/usr/share/doc/sysvinit-tools-2.88/COPYRIGHT",
      "/usr/share/man/man1/last.1.gz",
      "/usr/share/man/man1/lastb.1.gz",
      "/usr/share/man/man1/mesg.1.gz",
      "/usr/share/man/man1/utmpdump.1.gz",
      "/usr/share/man/man1/wall.1.gz",
      "/usr/share/man/man8/killall5.8.gz",
      "/usr/share/man/man8/pidof.8.gz",
      "/usr/share/man/man8/sulogin.8.gz",
      NULL
    };
    static const char *const sysvinit_files_9[] = {
      "/sbin/killall5",
      "/sbin/pidof",
      "/usr/bin/last",
      "/usr/bin/lastb",
      "/usr/bin/mesg",
      "/usr/bin/wall",
      "/usr/share/doc/sysvinit-tools-2.88",
      "/usr/share/doc/sysvinit-tools-2.88/Changelog",
      "/usr/share/doc/sysvinit-tools-2.88/COPYRIGHT",
      "/usr/share/man/man1/last.1.gz",
      "/usr/share/man/man1/lastb.1.gz",
      "/usr/share/man/man1/mesg.1.gz",
      "/usr/share/man/man1/wall.1.gz",
      "/usr/share/man/man8/killall5.8.gz",
      "/usr/share/man/man8/pidof.8.gz",
      NULL
    };

    pgconn_handle dbh(testdb.connect(DBNAME));
    std::vector<database::package_id> pids;
    pgresult_handle r1;
    r1.exec(dbh, "SELECT id, name, version, release FROM symboldb.package");
    for (int i = 0, endi = r1.ntuples(); i < endi; ++i) {
      {
	int pkg = 0;
	sscanf(r1.getvalue(i, 0), "%d", &pkg);
	CHECK(pkg > 0);
	pids.push_back(database::package_id(pkg));
      }
      COMPARE_STRING(r1.getvalue(i, 1), "sysvinit-tools");
      COMPARE_STRING(r1.getvalue(i, 2), "2.88");
      std::string release(r1.getvalue(i, 3));
      const char *const *files;
      if (starts_with(release, "6")) {
	files = sysvinit_files_6;
      } else if (starts_with(release, "9")) {
	files = sysvinit_files_9;
      } else {
	CHECK(false);
	break;
      }
      const char *params[] = {r1.getvalue(i, 0)};
      pgresult_handle r2;
      r2.execParams(dbh,
		    "SELECT name FROM symboldb.file"
		    " WHERE package = $1 ORDER BY 1", params);
      int endj = r2.ntuples();
      CHECK(endj > 0);
      for (int j = 0; j < endj; ++j) {
	CHECK(files[j] != NULL);
	if (files[j] == NULL) {
	  break;
	}
	COMPARE_STRING(r2.getvalue(j, 0), files[j]);
      }
      CHECK(files[endj] == NULL);
    }

    r1.exec(dbh,
	    "SELECT DISTINCT"
	    " length, user_name, group_name, mtime, mode,"
	    " encode(digest, 'hex'), encode(contents, 'hex'),"
	    " e_type, soname"
	    " FROM symboldb.file f"
	    " JOIN symboldb.package p ON f.package = p.id"
	    " JOIN symboldb.elf_file ef ON f.id = ef.file"
	    " WHERE f.name = '/sbin/killall5'"
	    " AND symboldb.nevra(p)"
	    " = 'sysvinit-tools-2.88-9.dsf.fc18.x86_64'");
    CHECK(r1.ntuples() == 1);
    COMPARE_STRING(r1.getvalue(0, 0), "23752");
    COMPARE_STRING(r1.getvalue(0, 1), "root");
    COMPARE_STRING(r1.getvalue(0, 2), "root");
    COMPARE_STRING(r1.getvalue(0, 3), "1347551182");
    COMPARE_STRING(r1.getvalue(0, 4), "33261"); // 0100755
    COMPARE_STRING(r1.getvalue(0, 5),
		   "b75fc6cd2359b0d7d3468be0499ca897"
		   "87234c72fe5b9cf36e4b28cd9a56025c");
    COMPARE_STRING(r1.getvalue(0, 6),
		   "7f454c46020101000000000000000000"
		   "03003e00010000009c1e000000000000"
		   "40000000000000008855000000000000"
		   "0000000040003800090040001d001c00");

    COMPARE_STRING(r1.getvalue(0, 7), "3"); // ET_DYN (sic)
    COMPARE_STRING(r1.getvalue(0, 8), "killall5");

    r1.exec(dbh,
	    "SELECT DISTINCT"
	    " length, user_name, group_name, mtime, mode,"
	    " encode(digest, 'hex'), encode(contents, 'hex'),"
	    " e_type, soname"
	    " FROM symboldb.file f"
	    " JOIN symboldb.package p ON f.package = p.id"
	    " JOIN symboldb.elf_file ef ON f.id = ef.file"
	    " WHERE f.name = '/usr/bin/wall'"
	    " AND symboldb.nevra(p)"
	    " = 'sysvinit-tools-2.88-9.dsf.fc18.x86_64'");
    CHECK(r1.ntuples() == 1);
    COMPARE_STRING(r1.getvalue(0, 0), "15352");
    COMPARE_STRING(r1.getvalue(0, 1), "root");
    COMPARE_STRING(r1.getvalue(0, 2), "tty");
    COMPARE_STRING(r1.getvalue(0, 3), "1347551181");
    COMPARE_STRING(r1.getvalue(0, 4), "34157"); // 0102555
    COMPARE_STRING(r1.getvalue(0, 5),
		   "36fdb67f4d549c4e13790ad836cb5641"
		   "af993ff28a3e623da4f95608653dc55a");
    COMPARE_STRING(r1.getvalue(0, 6),
		   "7f454c46020101000000000000000000"
		   "03003e0001000000cc18000000000000"
		   "4000000000000000b834000000000000"
		   "0000000040003800090040001d001c00");
    COMPARE_STRING(r1.getvalue(0, 7), "3"); // ET_DYN (sic)
    COMPARE_STRING(r1.getvalue(0, 8), "wall");
    r1.close();

    CHECK(!pids.empty());
    db.txn_begin();
    database::package_set_id pset(db.create_package_set("test-set", "x86_64"));
    CHECK(pset.value() > 0);
    CHECK(!db.update_package_set(pset, pids.begin(), pids.begin()));
    CHECK(db.update_package_set(pset, pids));
    CHECK(!db.update_package_set(pset, pids));
    db.txn_commit();

    r1.exec(dbh, "SELECT * FROM symboldb.package_set_member");
    CHECK(r1.ntuples() == static_cast<int>(pids.size()));

    db.txn_begin();
    CHECK(db.update_package_set(pset, pids.begin() + 1, pids.end()));
    CHECK(!db.update_package_set(pset, pids.begin() + 1, pids.end()));
    db.txn_commit();
    r1.exec(dbh, "SELECT * FROM symboldb.package_set_member");
    CHECK(r1.ntuples() == static_cast<int>(pids.size() - 1));
    char pkgstr[32];
    snprintf(pkgstr, sizeof(pkgstr), "%d", pids.front().value());
    {
      const char *params[] = {pkgstr};
      r1.execParams(dbh,
		    "SELECT * FROM symboldb.package_set_member"
		    " WHERE package = $1", params);
      CHECK(r1.ntuples() == 0);
    }
    db.txn_begin();
    CHECK(db.update_package_set(pset, pids.begin(), pids.begin() + 1));
    CHECK(!db.update_package_set(pset, pids.begin(), pids.begin() + 1));
    db.txn_commit();
    r1.exec(dbh, "SELECT package FROM symboldb.package_set_member");
    CHECK(r1.ntuples() == 1);
    COMPARE_STRING(r1.getvalue(0, 0), pkgstr);

    testdb.exec_test_sql(DBNAME, "DELETE FROM symboldb.package_set_member");
    char psetstr[32];
    snprintf(psetstr, sizeof(psetstr), "%d", pset.value());
    {
      const char *params[] = {psetstr};
      r1.execParams(dbh,
		    "INSERT INTO symboldb.package_set_member"
		    " SELECT $1, id FROM symboldb.package"
		    " WHERE arch IN ('x86_64', 'i686')", params);
    }
    r1.exec(dbh, "BEGIN");
    update_elf_closure(dbh, pset);
    r1.exec(dbh, "COMMIT");

    std::vector<std::vector<unsigned char> > digests;
    db.referenced_package_digests(digests);
    CHECK(digests.size() == 8); // 4 packages with 2 digests each
  }

  // FIXME: Add more sanity check on database contents.
}

static test_register t("rpm_load", test);
