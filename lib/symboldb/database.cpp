/*
 * Copyright (C) 2012, 2013 Red Hat, Inc.
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

#include <symboldb/database.hpp>
#include <symboldb/update_elf_closure.hpp>
#include <cxxll/rpm_file_info.hpp>
#include <cxxll/rpm_package_info.hpp>
#include <cxxll/elf_image.hpp>
#include <cxxll/elf_symbol_definition.hpp>
#include <cxxll/elf_symbol_reference.hpp>
#include <cxxll/pgconn_handle.hpp>
#include <cxxll/pgresult_handle.hpp>
#include <cxxll/pg_exception.hpp>
#include <cxxll/pg_query.hpp>
#include <cxxll/pg_response.hpp>
#include <cxxll/hash.hpp>
#include <cxxll/java_class.hpp>

#include <assert.h>
#include <stdlib.h>

#include <libpq-fe.h>

#include <algorithm>
#include <map>
#include <set>

using namespace cxxll;

// FIXME: We need to add a transaction runner, so that we can retry
// transactions on deadlock or update conflict.

// Database table names

#define PACKAGE_TABLE "symboldb.package"
#define PACKAGE_DIGEST_TABLE "symboldb.package_digest"
#define FILE_TABLE "symboldb.file"
#define FILE_CONTENTS_TABLE "symboldb.file_contents"
#define DIRECTORY_TABLE "symboldb.directory"
#define SYMLINK_TABLE "symboldb.symlink"
#define ELF_FILE_TABLE "symboldb.elf_file"
#define ELF_DEFINITION_TABLE "symboldb.elf_definition"
#define ELF_REFERENCE_TABLE "symboldb.elf_reference"
#define ELF_NEEDED_TABLE "symboldb.elf_needed"
#define ELF_RPATH_TABLE "symboldb.elf_rpath"
#define ELF_RUNPATH_TABLE "symboldb.elf_runpath"
#define ELF_ERROR_TABLE "symboldb.elf_error"
#define PACKAGE_SET_TABLE "symboldb.package_set"
#define PACKAGE_SET_MEMBER_TABLE "symboldb.package_set_member"
#define URL_CACHE_TABLE "symboldb.url_cache"

// Include the schema.sql file.
const char database::SCHEMA[] = {
#include "schema.sql.inc"
  , 0
};

struct database::impl {
  pgconn_handle conn;
};

database::database()
  : impl_(new impl)
{
  impl_->conn.reset(PQconnectdb(""));
}

database::database(const char *host, const char *dbname)
  : impl_(new impl)
{
  static const char *keys[] = {
    "host", "port", "dbname", NULL
  };
  const char *values[] = {
    host, "5432", dbname, NULL
  };
  impl_->conn.reset(PQconnectdbParams(keys, values, 0));
}

database::~database()
{
}

void
database::txn_begin()
{
  pgresult_handle res;
  res.exec(impl_->conn, "BEGIN");
}

void
database::txn_commit()
{
  pgresult_handle res;
  res.exec(impl_->conn, "COMMIT");
}

void
database::txn_rollback()
{
  pgresult_handle res;
  res.exec(impl_->conn, "ROLLBACK");
}

void
database::txn_begin_no_sync()
{
  pgresult_handle res;
  res.exec(impl_->conn, "BEGIN; SET LOCAL synchronous_commit TO OFF");
}

database::advisory_lock_guard::~advisory_lock_guard()
{
}

struct database::advisory_lock_impl : database::advisory_lock_guard {
  std::tr1::shared_ptr<database::impl> impl_;
  int a;
  int b;
  virtual ~advisory_lock_impl();
};

database::advisory_lock_impl::~advisory_lock_impl()
{
  try {
    pgresult_handle res;
    pg_query(impl_->conn, res, "SELECT pg_advisory_unlock($1, $2)", a, b);
  } catch(...) {
    // TODO: Not much we can do here.  Logging would be useful.
  }
}

database::advisory_lock
database::lock(int a, int b)
{
  pgresult_handle res;
  if (impl_->conn.transactionStatus() == PQTRANS_INTRANS) {
    pg_query(impl_->conn, res, "SELECT pg_advisory_xact_lock($1, $2)", a, b);
    // As this is a NOP, we do not have to guard against exceptions
    // from the object allocation.
    return advisory_lock(new advisory_lock_guard);
  } else {
    // Allocate beforehand to avoid exceptions after acquiring the
    // lock.
    std::tr1::shared_ptr<advisory_lock_impl> lock(new advisory_lock_impl);
    pg_query(impl_->conn, res,  "SELECT pg_advisory_lock($1, $2)", a, b);
    lock->impl_ = impl_;
    lock->a = a;
    lock->b = b;
    return lock;
  }
}


static int
get_id(pgresult_handle &res)
{
  if (res.ntuples() > 0) {
    int id;
    pg_response(res, 0, id);
    if (id <= 0) {
      throw pg_exception("database returned invalid ID");
    }
    return id;
  }
  return 0;
}

static int
get_id_force(pgresult_handle &res)
{
  int id = get_id(res);
  if (id <= 0) {
    throw pg_exception("unexpected empty result set");
  }
  return id;
}

bool
database::intern_package(const rpm_package_info &pkg,
			 package_id &pkg_id)
{
  // FIXME: This needs a transaction and locking.

  // Try to locate existing row.
  pgresult_handle res;
  pg_query_binary
    (impl_->conn, res,
     "SELECT package_id FROM " PACKAGE_TABLE " WHERE hash = decode($1, 'hex')",
     pkg.hash);
  int id = get_id(res);
  if (id > 0) {
    pkg_id = package_id(id);
    return false;
  }

  // Insert new row.
  pg_query_binary
    (impl_->conn, res,
     "INSERT INTO " PACKAGE_TABLE
     " (name, epoch, version, release, arch, hash, source,"
     " build_host, build_time)"
     " VALUES ($1, $2, $3, $4, $5::symboldb.rpm_arch, decode($6, 'hex'), $7,"
     " $8, 'epoch'::TIMESTAMP WITHOUT TIME ZONE + '1 second'::interval * $9)"
     " RETURNING package_id",
     pkg.name, pkg.epoch >= 0 ? &pkg.epoch : NULL, pkg.version, pkg.release,
     pkg.arch, pkg.hash, pkg.source_rpm, pkg.build_host, pkg.build_time);
  pkg_id = package_id(get_id_force(res));
  return true;
}

static void
intern_hash(const rpm_file_info &info,
	    const std::vector<unsigned char> &digest,
	    std::vector<unsigned char> &result)
{
  std::vector<unsigned char> to_hash(digest);
  union {
    unsigned mtime;
    char data_bytes[sizeof(mtime)];
  } u;
  u.mtime = cpu_to_le_32(info.mtime);
  to_hash.insert(to_hash.end(),
		 u.data_bytes + 0, u.data_bytes + sizeof(u.data_bytes));
  to_hash.insert(to_hash.end(),
		 info.user.begin(), info.user.end());
  to_hash.push_back('\0');
  to_hash.insert(to_hash.end(),
		 info.group.begin(), info.group.end());
  result = hash(hash_sink::md5, to_hash);
}

bool
database::intern_file_contents(const rpm_file_info &info,
			       const std::vector<unsigned char> &digest,
			       const std::vector<unsigned char> &contents,
			       contents_id &cid)
{
  // FIXME: This needs a transaction.
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  long long length = info.digest.length;
  if (length < 0) {
    std::runtime_error("file length out of range");
  }
  int mode = info.mode;
  if (mode < 0) {
    std::runtime_error("file mode out of range");
  }

  std::vector<unsigned char> row_hash;
  intern_hash(info, digest, row_hash);

  // Ideally, we would like to obtain a lock here, but for large RPM
  // packages, the required number of locks would be huge.
  pgresult_handle res;
  pg_query_binary
    (impl_->conn, res,
     "SELECT * FROM symboldb.intern_file_contents($1, $2, $3, $4, $5, $6, $7)",
     row_hash, length, mode, info.user, info.group, digest, contents);
  int id;
  bool added;
  pg_response(res, 0, id, added);
  cid = contents_id(id);
  return added;
}

void
database::add_package_digest(package_id pkg,
			     const std::vector<unsigned char> &digest,
			     unsigned long long length)
{
  // FIXME: This needs a transaction and locking.

  if (digest.size() < 16) {
    throw std::logic_error("invalid digest length");
  }
  if (length > (1ULL << 60)) {
    throw std::logic_error("invalid length");
  }

  // Try to locate existing row.
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "SELECT 1 FROM " PACKAGE_DIGEST_TABLE
     " WHERE package_id = $1 AND digest = $2 AND length = $3",
     pkg.value(), digest, static_cast<long long>(length));
  if (res.ntuples() > 0) {
    return;
  }

  // Insert new row.
  pg_query
    (impl_->conn, res,
     "INSERT INTO " PACKAGE_DIGEST_TABLE " (package_id, digest, length)"
     " VALUES ($1, $2, $3)",
     pkg.value(), digest, static_cast<long long>(length));
}

database::package_id
database::package_by_digest(const std::vector<unsigned char> &digest)
{
  if (digest.size() < 16) {
    throw std::logic_error("invalid digest length");
  }
  pgresult_handle res;
  pg_query_binary
    (impl_->conn, res,
     "SELECT package_id FROM " PACKAGE_DIGEST_TABLE " WHERE digest = $1",
     digest);
  return package_id(get_id(res));
}

database::file_id
database::add_file(package_id pkg, const std::string &name, bool normalized,
		   long long mtime, int inode, contents_id cid)
{
  // FIXME: This needs a transaction.
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  pgresult_handle res;
  pg_query_binary
    (impl_->conn, res,
     "INSERT INTO " FILE_TABLE " (package_id, name, mtime, inode,"
     " contents_id, normalized)"
     " VALUES ($1, $2, $3, $4, $5, $6) RETURNING file_id",
     pkg.value(), name, mtime, inode, cid.value(), normalized);
  return file_id(get_id_force(res));
}

void
database::add_file(package_id pkg, const cxxll::rpm_file_info &info,
		   const std::vector<unsigned char> &digest,
		   const std::vector<unsigned char> &contents,
		   file_id &fid, contents_id &cid, bool &added)
{
  // FIXME: This needs a transaction.
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  long long length = info.digest.length;
  if (length < 0) {
    std::runtime_error("file length out of range");
  }
  int mode = info.mode;
  if (mode < 0) {
    std::runtime_error("file mode out of range");
  }
  int ino = info.ino;
  if (ino < 0) {
    std::runtime_error("file inode out of range");
  }
  int mtime = info.mtime;
  if (mtime < 0) {
    std::runtime_error("file mtime out of range");
  }

  std::vector<unsigned char> row_hash;
  intern_hash(info, digest, row_hash);

  pgresult_handle res;
  pg_query_binary
    (impl_->conn, res,
     "SELECT * FROM symboldb.add_file"
     "($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)",
     row_hash, length, mode, info.user, info.group, digest, contents,
     pkg.value(), ino, mtime, info.name, info.normalized);
  int fidint;
  int cidint;
  pg_response(res, 0, fidint, cidint, added);
  fid = file_id(fidint);
  cid = contents_id(cidint);
}

void
database::add_directory(package_id pkg, const rpm_file_info &info)
{
  // FIXME: This needs a transaction.
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "INSERT INTO " DIRECTORY_TABLE
     " (package_id, name, user_name, group_name, mtime, mode, normalized)"
     " VALUES ($1, $2, $3, $4, $5, $6, $7)",
     pkg.value(), info.name, info.user, info.group,
     static_cast<long long>(info.mtime),
     static_cast<long long>(info.mode),
     info.normalized);
}

void
database::add_symlink(package_id pkg, const rpm_file_info &info,
		      const std::vector<unsigned char> &contents)
{
  // FIXME: This needs a transaction.
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  assert(info.is_symlink());
  std::string target(contents.begin(), contents.end());
  if (target.empty() || target.find('\0') != std::string::npos) {
    throw std::runtime_error("symlink with invalid target");
  }
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "INSERT INTO " SYMLINK_TABLE
     " (package_id, name, target, user_name, group_name, mtime, normalized)"
     " VALUES ($1, $2, $3, $4, $5, $6, $7)",
     pkg.value(), info.name, target, info.user, info.group,
     static_cast<long long>(info.mtime),
     info.normalized);
}

void
database::add_elf_image(contents_id cid, const elf_image &image,
			const char *soname)
{
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "INSERT INTO " ELF_FILE_TABLE
     " (contents_id, ei_class, ei_data, e_type, e_machine, arch, soname,"
     " build_id)"
     " VALUES ($1, $2, $3, $4, $5, $6::symboldb.elf_arch, $7, $8)",
     cid.value(),
     static_cast<int>(image.ei_class()),
     static_cast<int>(image.ei_data()),
     static_cast<int>(image.e_type()),
     static_cast<int>(image.e_machine()),
     image.arch(),
     soname,
     image.build_id().empty() ? NULL : &image.build_id());
}

void
database::add_elf_symbol_definition(contents_id cid,
				    const elf_symbol_definition &def)
{
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  pgresult_handle res;
  int xsection = def.xsection;
  pg_query
    (impl_->conn, res,
     "INSERT INTO " ELF_DEFINITION_TABLE
     " (contents_id, name, version, primary_version, symbol_type, binding,"
     " section, xsection, visibility)"
     " VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9::symboldb.elf_visibility)",
     cid.value(),
     def.symbol_name.c_str(),
     def.vda_name.empty() ? NULL : def.vda_name.c_str(),
     def.default_version,
     static_cast<int>(def.type),
     static_cast<int>(def.binding),
     static_cast<short>(def.section),
     def.has_xsection() ? &xsection : NULL,
     def.visibility());
}

void
database::add_elf_symbol_reference(contents_id cid,
				   const elf_symbol_reference &ref)
{
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "INSERT INTO " ELF_REFERENCE_TABLE
     " (contents_id, name, version, symbol_type, binding, visibility)"
     " VALUES ($1, $2, $3, $4, $5, $6::symboldb.elf_visibility)",
     cid.value(),
     ref.symbol_name,
     ref.vna_name.empty() ? NULL : ref.vna_name.c_str(),
     static_cast<int>(ref.type),
     static_cast<int>(ref.binding),
     ref.visibility());
}

void
database::add_elf_needed(contents_id cid, const char *name)
{
  // FIXME: This needs a transaction.
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "INSERT INTO " ELF_NEEDED_TABLE " (contents_id, name) VALUES ($1, $2)",
     cid.value(), name);
}

void
database::add_elf_rpath(contents_id cid, const char *name)
{
  // FIXME: This needs a transaction.
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "INSERT INTO " ELF_RPATH_TABLE " (contents_id, path) VALUES ($1, $2)",
     cid.value(), name);
}

void
database::add_elf_runpath(contents_id cid, const char *name)
{
  // FIXME: This needs a transaction.
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "INSERT INTO " ELF_RUNPATH_TABLE " (contents_id, path) VALUES ($1, $2)",
     cid.value(), name);
}

void
database::add_elf_error(contents_id cid, const char *message)
{
  // FIXME: This needs a transaction.
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "INSERT INTO " ELF_ERROR_TABLE " (contents_id, message) VALUES ($1, $2)",
     cid.value(), message);
}

//////////////////////////////////////////////////////////////////////
// Java classes.

void
database::add_java_class(contents_id cid, const cxxll::java_class &jc)
{
  // FIXME: This needs a transaction.
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  pgresult_handle res;
  std::vector<unsigned char> digest(hash(hash_sink::sha256, jc.buffer()));
  std::string this_class(jc.this_class());
  pg_query_binary
    (impl_->conn, res, "SELECT * FROM symboldb.intern_java_class"
     " ($1, $2, $3, $4)", digest,
     this_class, jc.super_class(), static_cast<int>(jc.access_flags()));
  int classid;
  bool added;
  pg_response(res, 0, classid, added);
  if (added) {
    for (unsigned i= 0, end = jc.interface_count(); i < end; ++i) {
      pg_query
	(impl_->conn, res,
	 "INSERT INTO symboldb.java_interface (class_id, name) VALUES ($1, $2)",
	 classid, jc.interface(i));
    }
    std::vector<std::string> classes(jc.class_references());
    std::sort(classes.begin(), classes.end());
    std::vector<std::string>::iterator end =
      std::unique(classes.begin(), classes.end());
    for (std::vector<std::string>::iterator p = classes.begin();
	 p != end; ++p) {
      const std::string &name(*p);
      if (name != "java/lang/Object" && name != "java/lang/String"
	  && name != this_class) {
	pg_query(impl_->conn, res,
		 "INSERT INTO symboldb.java_class_reference (class_id, name)"
		 " VALUES ($1, $2)", classid, name);
      }
    }
  }
  pg_query
    (impl_->conn, res,
     "INSERT INTO symboldb.java_class_contents"
     " (class_id, contents_id) VALUES ($1, $2)", classid, cid.value());
}

void
database::add_java_error(contents_id cid,
			 const char *message, const std::string &path)
{
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "INSERT INTO symboldb.java_error (contents_id, message, path)"
     " VALUES ($1, $2, $3)", cid.value(), message, path);
}


//////////////////////////////////////////////////////////////////////
// Package sets.

database::package_set_id
database::create_package_set(const char *name)
{
  pgresult_handle res;
  pg_query_binary
    (impl_->conn, res,
     "INSERT INTO " PACKAGE_SET_TABLE
     " (name) VALUES ($1) RETURNING set_id", name);
  return package_set_id(get_id_force(res));
}

database::package_set_id
database::lookup_package_set(const char *name)
{
  pgresult_handle res;
  pg_query_binary
    (impl_->conn, res,
     "SELECT set_id FROM " PACKAGE_SET_TABLE " WHERE name = $1", name);
  return package_set_id(get_id(res));
}

void
database::add_package_set(package_set_id set, package_id pkg)
{
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "INSERT INTO " PACKAGE_SET_MEMBER_TABLE
     " (set_id, package_id) VALUES ($1, $2)", set.value(), pkg.value());
}

void
database::delete_from_package_set(package_set_id set, package_id pkg)
{
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "DELETE FROM " PACKAGE_SET_MEMBER_TABLE
     " WHERE set_id = $1 AND package_id = $2",
     set.value(), pkg.value());
}

void
database::empty_package_set(package_set_id set)
{
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "DELETE FROM " PACKAGE_SET_MEMBER_TABLE " WHERE set_id = $1", set.value());
}

bool
database::update_package_set(package_set_id set,
			     const std::vector<package_id> &pids)
{
  assert(impl_->conn.transactionStatus() == PQTRANS_INTRANS);
  bool changes = false;

  std::set<package_id> old;
  {
    pgresult_handle res;
    pg_query_binary
      (impl_->conn, res, "SELECT package_id FROM " PACKAGE_SET_MEMBER_TABLE
       " WHERE set_id = $1", set.value());

    for (int row = 0, end = res.ntuples(); row < end; ++row) {
      int pkg;
      pg_response(res, row, pkg);
      assert(pkg != 0);
      old.insert(package_id(pkg));
    }
  }

  for (std::vector<package_id>::const_iterator
	 p = pids.begin(), end = pids.end(); p != end; ++p) {
    package_id pkg = *p;
    if (old.erase(pkg) == 0) {
      // New package set member.
      add_package_set(set, pkg);
      changes = true;
    }
  }

  // Remaining old entries have to be deleted.
  for (std::set<package_id>::const_iterator
	 p = old.begin(), end = old.end(); p != end; ++p) {
    delete_from_package_set(set, *p);
    changes = true;
  }

  return changes;
}

void
database::update_package_set_caches(package_set_id set)
{
  update_elf_closure(impl_->conn, set, NULL);
}

bool
database::url_cache_fetch(const char *url, size_t expected_length,
			  long long expected_time,
			  std::vector<unsigned char> &data)
{
  if (expected_length > 1U << 30) {
    return false;
  }
  pgresult_handle res;
  pg_query_binary
    (impl_->conn, res, "SELECT data FROM " URL_CACHE_TABLE
     " WHERE url = $1 AND LENGTH(data) = $2 AND http_time = $3",
     url, static_cast<int>(expected_length), expected_time);
  if (res.ntuples() != 1) {
    return false;
  } else {
    pg_response(res, 0, data);
    return true;
  }
}

bool
database::url_cache_fetch(const char *url,
			  std::vector<unsigned char> &data)
{
  pgresult_handle res;
  pg_query_binary
    (impl_->conn, res,
     "SELECT data FROM " URL_CACHE_TABLE " WHERE url = $1", url);
  if (res.ntuples() != 1) {
    return false;
  }
  pg_response(res, 0, data);
  return true;
}

void
database::url_cache_update(const char *url,
			   const std::vector<unsigned char> &data,
			   long long time)
{
  pgresult_handle res;
  pg_query
    (impl_->conn, res,
     "SELECT 1 FROM " URL_CACHE_TABLE " WHERE url = $1 FOR UPDATE", url);

  if (res.ntuples() == 1) {
    pg_query
      (impl_->conn, res, "UPDATE " URL_CACHE_TABLE
       " SET http_time = $2, data = $3, last_change = NOW() AT TIME ZONE 'UTC'"
       " WHERE url = $1", url, time, data);
  } else {
    pg_query
      (impl_->conn, res, "INSERT INTO " URL_CACHE_TABLE
       " (url, http_time, data, last_change)"
       " VALUES ($1, $2, $3, NOW() AT TIME ZONE 'UTC')", url, time, data);
  }
}

void
database::referenced_package_digests
  (std::vector<std::vector<unsigned char> > &digests)
{
  pgresult_handle res;
  res.execBinary
    (impl_->conn,
     "SELECT digest FROM " PACKAGE_SET_MEMBER_TABLE
     " JOIN " PACKAGE_DIGEST_TABLE " USING (package_id)"
     " ORDER BY digest");
  std::vector<unsigned char> digest;
  for (int i = 0, end = res.ntuples(); i < end; ++i) {
    pg_response(res, i, digest);
    digests.push_back(digest);
  }
}

void
database::expire_packages()
{
  pgresult_handle res;
  res.exec
    (impl_->conn, "DELETE FROM symboldb.package p"
     " WHERE NOT EXISTS (SELECT 1 FROM symboldb.package_set_member psm"
     " WHERE psm.package_id = p.package_id LIMIT 1)");
}

void
database::expire_file_contents()
{
  pgresult_handle res;
  res.exec
    (impl_->conn, "DELETE FROM symboldb.file_contents fc"
     " WHERE NOT EXISTS (SELECT 1 FROM symboldb.file f"
     " WHERE f.contents_id = fc.contents_id LIMIT 1)");
}

void
database::expire_java_classes()
{
  pgresult_handle res;
  res.exec
    (impl_->conn, "DELETE FROM symboldb.java_class jc"
     " WHERE NOT EXISTS (SELECT 1 FROM symboldb.java_class_contents j"
     " WHERE j.class_id = jc.class_id LIMIT 1)");
}

namespace {
  struct fc_entry {
    std::string file;
    std::string nevra;
  };
}

void
database::print_elf_soname_conflicts(package_set_id set)
{
  struct dumper : update_elf_closure_conflicts {
    database *db;
    dumper(database *dbase)
      : db(dbase)
    {
    }

    void missing(file_id fid, const std::string &soname)
    {
      const fc_entry &entry(get_name(fid));
      printf("missing: %s (%s) %s\n",
	     entry.file.c_str(), entry.nevra.c_str(), soname.c_str());
    }

    void conflict(file_id fid, const std::string &soname,
		  const std::vector<file_id> &choices)
    {
      {
	const fc_entry &entry(get_name(fid));
	printf("conflicts: %s (%s) %s\n",
	       entry.file.c_str(), entry.nevra.c_str(), soname.c_str());
      }
      const char *first = "*";
      for (std::vector<file_id>::const_iterator
	     p = choices.begin(), end = choices.end(); p != end; ++p) {
	const fc_entry &entry(get_name(*p));
	printf("  %s %s (%s)\n",
	       first, entry.file.c_str(), entry.nevra.c_str());
	first = " ";
      }
    }

    bool skip_update()
    {
      return true;
    }

    typedef std::map<file_id, fc_entry> file_cache;
    file_cache file_cache_;

    const fc_entry &get_name(file_id fid)
    {
      file_cache::iterator p = file_cache_.find(fid);
      if (p != file_cache_.end()) {
	return p->second;
      }

      fc_entry &entry(file_cache_[fid]);
      pgresult_handle res;
      pg_query_binary
	(db->impl_->conn, res,
	 "SELECT f.name, symboldb.nevra(p)"
	 " FROM symboldb.file f JOIN symboldb.package p USING (package_id)"
	 " WHERE f.file_id = $1", fid.value());
      if (res.ntuples() != 1) {
	throw std::runtime_error("could not locate symboldb.file row");
      }
      pg_response(res, 0, entry.file, entry.nevra);
      return entry;
    }
  } dumper(this);

  pgresult_handle res;
  res.exec(impl_->conn,
	   "BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY");
  update_elf_closure(impl_->conn, set, &dumper);
  res.exec(impl_->conn, "ROLLBACK");
}

void
database::exec_sql(const char *command)
{
  pgresult_handle res;
  res.exec(impl_->conn, command);
}

void
database::create_schema()
{
  exec_sql(SCHEMA);
}

//////////////////////////////////////////////////////////////////////
// database::file_with_digest

struct database::files_with_digest::impl {
  std::tr1::shared_ptr<database::impl> impl_;
  std::vector<unsigned char> rpm_digest_;
  std::string file_name_;
  pgresult_handle res_;
  int row_;
  impl(database &, const std::vector<unsigned char> &);
};

database::files_with_digest::impl::impl
  (database &db, const std::vector<unsigned char> &digest)
  : impl_(db.impl_), row_(0)
{
  pg_query_binary
    (impl_->conn, res_,
     "SELECT pd.digest, f.name"
     " FROM symboldb.package_digest pd"
     " JOIN symboldb.file f USING (package_id)"
     " JOIN symboldb.file_contents fc USING (contents_id)"
     " WHERE fc.digest = $1", digest);
}

database::files_with_digest::files_with_digest
  (database &db, const std::vector<unsigned char> &digest)
  : impl_(new impl(db, digest))
{
}

database::files_with_digest::~files_with_digest()
{
}

bool
database::files_with_digest::next()
{
  if (impl_->row_ <= impl_->res_.ntuples()) {
    pg_response(impl_->res_, impl_->row_,
		impl_->rpm_digest_, impl_->file_name_);
    ++impl_->row_;
    return true;
  }
  return false;
}

const std::vector<unsigned char> &
database::files_with_digest::rpm_digest() const
{
  return impl_->rpm_digest_;
}

const std::string &
database::files_with_digest::file_name() const
{
  return impl_->file_name_;
}
