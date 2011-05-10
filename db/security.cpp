// security.cpp

/**
 *    Copyright (C) 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "security.h"
#include "instance.h"
#include "client.h"
#include "curop-inl.h"
#include "db.h"
#include "dbhelpers.h"

namespace mongo {

    bool AuthenticationInfo::_warned = false;

    void AuthenticationInfo::print() const {
        cout << "AuthenticationInfo: " << this << '\n';
        for ( MA::const_iterator i=_dbs.begin(); i!=_dbs.end(); i++ ) {
            cout << "\t" << i->first << "\t" << i->second.level << '\n';
        }
        cout << "END" << endl;
    }


    string AuthenticationInfo::getUser( const string& dbname ) const {
        scoped_spinlock lk(_lock);

        MA::const_iterator i = _dbs.find(dbname);
        if ( i == _dbs.end() )
            return "";

        return i->second.user;
    }

    bool AuthenticationInfo::_isAuthorized(const string& dbname, int level) const {
        {
            scoped_spinlock lk(_lock);
            
            if ( _isAuthorizedSingle_inlock( dbname , level ) )
                return true;
            
            if ( noauth ) 
                return true;
            
            if ( _isAuthorizedSingle_inlock( "admin" , level ) )
                return true;
            
            if ( _isAuthorizedSingle_inlock( "local" , level ) )
                return true;
        }
        return _isAuthorizedSpecialChecks( dbname );
    }

    bool AuthenticationInfo::_isAuthorizedSingle_inlock(const string& dbname, int level) const {
        MA::const_iterator i = _dbs.find(dbname);
        return i != _dbs.end() && i->second.level >= level;
    }

    bool AuthenticationInfo::_isAuthorizedSpecialChecks( const string& dbname ) const {
        if ( cc().isGod() ) 
            return true;

        if ( isLocalHost ) {
            atleastreadlock l("");
            Client::GodScope gs;
            Client::Context c("admin.system.users");
            BSONObj result;
            if( ! Helpers::getSingleton("admin.system.users", result) ) {
                if( ! _warned ) {
                    // you could get a few of these in a race, but that's ok
                    _warned = true;
                    log() << "note: no users configured in admin.system.users, allowing localhost access" << endl;
                }
                return true;
            }
        }

        return false;
    }

} // namespace mongo

