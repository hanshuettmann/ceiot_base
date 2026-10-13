const express = require("express");
const bodyParser = require("body-parser");
const { MongoClient } = require("mongodb");
const PgMem = require("pg-mem");

const db = PgMem.newDb();

//Agregamos un adaptador
const { Pool } = db.adapters.createPg();
const sqlClient = new Pool();

const render = require("./render.js");
// Measurements database setup and access

let database = null;
const collectionName = "measurements";

async function startDatabase() {
    const uri = "mongodb://localhost:27017/?maxPoolSize=20&w=majority";
    const connection = await MongoClient.connect(uri);
    database = connection.db();
}

async function getDatabase() {
    if (!database) await startDatabase();
    return database;
}

async function insertMeasurement(message) {
    const { insertedId } = await database.collection(collectionName).insertOne(message);
    return insertedId;
}

async function getMeasurements() {
    return await database.collection(collectionName).find({}).toArray();
}

// API Server

const app = express();

app.use(bodyParser.urlencoded({ extended: false }));
app.use(express.static('spa/static'));
//Agregamos para recibir json
app.use(express.json());

const PORT = 8080;

//Le damos un poco de cariño al endpoint
app.post('/measurement', async function (req, res) {
    try {
        const { id, t, h, p } = req.body;
        console.log(
            `device id: ${id}, temperature: ${t}, humidity: ${h}, pressure: ${p}`
        );

        const insertedId = await insertMeasurement({ id, t, h, p });

        res.status(201).json({
            message: "Measurement received",
            insertedId
        });

    } catch (error) {
        console.error("Error inserting measurement: ", error);

        res.status(500).json({
            error: "Could not insert measurement"
        });
    }
});

app.post('/device', function (req, res) {
    console.log("device id    : " + req.body.id + " name        : " + req.body.n + " key         : " + req.body.k);

    db.public.none("INSERT INTO devices VALUES ('" + req.body.id + "', '" + req.body.n + "', '" + req.body.k + "')");
    res.send("received new device");
});

// Update device, patch para permitir updates parciales
app.patch('/device/:id', async function (req, res) {
    const { n, k } = req.body;
    const { id } = req.params;

    try {
        const result = await sqlClient.query(
            // COALESCE($1, name) -> si $1 es nuevo, actualiza name
            // COALESCE($1, name) -> si $1 es null, conserva name
            `UPDATE devices
             SET name = COALESCE($1, name),
                 key = COALESCE($2, key)
             WHERE device_id = $3
             RETURNING device_id, name`,
            [n ?? null, k ?? null, id]
        );

        if (result.rows.length !== 1) return res.status(404).send({ error: "Could not find device" });

        res.send(result.rows[0]);

    } catch {
        res.status(500).send({ error: "Could not update device" });
    }

});


app.get('/web/device', function (req, res) {
    var devices = db.public.many("SELECT * FROM devices").map(function (device) {
        console.log(device);
        return '<tr><td><a href=/web/device/' + device.device_id + '>' + device.device_id + "</a>" +
            "</td><td>" + device.name + "</td><td>" + device.key + "</td></tr>";
    }
    );
    res.send("<html>" +
        "<head><title>Sensores</title></head>" +
        "<body>" +
        "<table border=\"1\">" +
        "<tr><th>id</th><th>name</th><th>key</th></tr>" +
        devices +
        "</table>" +
        "</body>" +
        "</html>");
});

app.get('/web/device/:id', function (req, res) {
    var template = "<html>" +
        "<head><title>Sensor {{name}}</title></head>" +
        "<body>" +
        "<h1>{{ name }}</h1>" +
        "id  : {{ id }}<br/>" +
        "Key : {{ key }}" +
        "</body>" +
        "</html>";


    var device = db.public.many("SELECT * FROM devices WHERE device_id = '" + req.params.id + "'");
    console.log(device);
    res.send(render(template, { id: device[0].device_id, key: device[0].key, name: device[0].name }));
});


app.get('/term/device/:id', function (req, res) {
    var red = "\33[31m";
    var green = "\33[32m";
    var blue = "\33[33m";
    var reset = "\33[0m";
    var template = "Device name " + red + "   {{name}}" + reset + "\n" +
        "       id   " + green + "       {{ id }} " + reset + "\n" +
        "       key  " + blue + "  {{ key }}" + reset + "\n";
    var device = db.public.many("SELECT * FROM devices WHERE device_id = '" + req.params.id + "'");
    console.log(device);
    res.send(render(template, { id: device[0].device_id, key: device[0].key, name: device[0].name }));
});

app.get('/measurement', async (req, res) => {
    res.send(await getMeasurements());
});

app.get('/device', function (req, res) {
    res.send(db.public.many("SELECT * FROM devices"));
});

startDatabase().then(async () => {

    const addAdminEndpoint = require("./admin.js");
    addAdminEndpoint(app, render);

    await insertMeasurement({ id: '00', t: '18', h: '78' });
    await insertMeasurement({ id: '00', t: '19', h: '77' });
    await insertMeasurement({ id: '00', t: '17', h: '77' });
    await insertMeasurement({ id: '01', t: '17', h: '77' });
    console.log("mongo measurement database Up");

    db.public.none("CREATE TABLE devices (device_id VARCHAR, name VARCHAR, key VARCHAR)");
    db.public.none("INSERT INTO devices VALUES ('00', 'Fake Device 00', '123456')");
    db.public.none("INSERT INTO devices VALUES ('01', 'Fake Device 01', '234567')");
    db.public.none("CREATE TABLE users (user_id VARCHAR, name VARCHAR, key VARCHAR)");
    db.public.none("INSERT INTO users VALUES ('1','Ana','admin123')");
    db.public.none("INSERT INTO users VALUES ('2','Beto','user123')");

    console.log("sql device database up");

    app.listen(PORT, () => {
        console.log(`Listening at ${PORT}`);
    });
});
