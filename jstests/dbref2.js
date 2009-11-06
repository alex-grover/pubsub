
a = db.dbref1a;
b = db.dbref1b;

a.drop();
b.drop();

a.save( { name : "eliot" } );
b.save( { num : 1 , link : new DBRef( "dbref1a" , a.findOne()._id ) } );
assert.eq( "eliot" , b.findOne().link.fetch().name , "A" );

assert.eq( 1 , b.find( function(){ return this.link.fetch().name == "eliot"; } ).count() , "B" );
assert.eq( 0 , b.find( function(){ return this.link.fetch().name == "el"; } ).count() , "C" );
