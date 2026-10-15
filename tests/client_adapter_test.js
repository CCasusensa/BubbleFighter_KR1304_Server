// Offline Frida model: no injection, process spawn, native UI, or sockets.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const directory = path.join(__dirname, '../tools/client_runtime');
const profile = JSON.parse(fs.readFileSync(path.join(directory, 'profiles.json'), 'utf8'));
let checks = 0;
function check(value, message) { checks++; assert.ok(value, message); }
function equal(actual, expected) { checks++; assert.deepEqual(actual, expected); }
function throws(action) { checks++; assert.throws(action); }

function fixture() {
    const memory = new Map(), replacements = new Map(), attaches = new Map(), natives = new Map();
    const events = [], calls = [], sent = [], passthrough = [], patches = [];
    let heap = 0x3000000;
    class Pointer {
        constructor(value) { this.value = Number(value instanceof Pointer ? value.value : value) >>> 0; }
        add(value) { return ptr(this.value + Number(value instanceof Pointer ? value.value : value)); }
        sub(value) { return ptr(this.value - Number(value instanceof Pointer ? value.value : value)); }
        and(value) { return ptr(this.value & ptr(value).value); }
        equals(value) { return this.value === ptr(value).value; }
        isNull() { return this.value === 0; }
        toString() { return '0x' + this.value.toString(16); }
        toInt32() { return this.value | 0; }
        toUInt32() { return this.value; }
        readByteArray(length) { return Uint8Array.from({length}, (_, i) => memory.get(this.value + i) || 0).buffer; }
        writeByteArray(bytes) { Array.from(bytes).forEach((v, i) => memory.set(this.value + i, v)); return this; }
        readU8() { return memory.get(this.value) || 0; }
        readU16() { return new DataView(this.readByteArray(2)).getUint16(0, true); }
        readU32() { return new DataView(this.readByteArray(4)).getUint32(0, true); }
        readS32() { return this.readU32() | 0; }
        writeU8(value) { memory.set(this.value, value & 255); return this; }
        writeU16(value) { const b = Buffer.alloc(2); b.writeUInt16LE(value); return this.writeByteArray(b); }
        writeU32(value) { const b = Buffer.alloc(4); b.writeUInt32LE(Number(value) >>> 0); return this.writeByteArray(b); }
        writePointer(value) { return this.writeU32(ptr(value).value); }
        readPointer() { return ptr(this.readU32()); }
        readUtf16String(length = 65536) {
            let text = '';
            for (let i = 0; i < length; i++) { const c = this.add(i * 2).readU16(); if (!c) break; text += String.fromCharCode(c); }
            return text;
        }
        writeUtf16String(text) { return this.writeByteArray(Buffer.from(text + '\0', 'utf16le')); }
        readUtf8String() { let s = ''; for (let i = 0; i < 4096 && this.add(i).readU8(); i++) s += String.fromCharCode(this.add(i).readU8()); return s; }
    }
    const ptr = value => new Pointer(value);
    const alloc = count => { const p = ptr(heap); heap += count + 64; return p; };
    const all = [profile.Ticket, ...profile.Messenger.Points, ...profile.Handoff.Points, ...profile.Warehouse, ...profile.Instance];
    for (const p of all) ptr(p.Va).writeByteArray(Buffer.from(p.Bytes, 'hex'));
    for (const p of [profile.ShopFeatures.Shop, profile.ShopFeatures.CashPurchase])
        ptr(0x400000 + Number(p.Rva)).writeByteArray(Buffer.from(p.ExpectedBytes, 'hex'));
    const vtable = ptr(0x163b680), writeAddress = ptr(0x2200000);
    vtable.add(48).writePointer(writeAddress);
    natives.set(writeAddress.value, (p, data, length) => {
        const start = p.add(24).readPointer();
        start.add(8).writeByteArray(new Uint8Array(data.readByteArray(length)));
        p.add(28).writePointer(start.add(8 + length));
        return 1;
    });
    natives.set(0x10bb360, p => {
        const start = alloc(4096); p.add(24).writePointer(start); p.add(28).writePointer(start.add(8)); return p;
    });
    natives.set(0x961940, (_, p) => {
        const begin = p.add(24).readPointer(), size = p.add(28).readPointer().sub(begin).toInt32();
        sent.push({id: begin.add(4).readU16(), payload: Buffer.from(begin.add(8).readByteArray(size - 8))}); return 1;
    });
    natives.set(0x9d2ff0, (_, p) => { passthrough.push(p); return 7; });
    natives.set(0x122ffbc, size => alloc(size));
    natives.set(0xf8f1e0, () => 17);
    const context = {
        rpc: {exports: {}}, ptr, Uint8Array, Set, Map,
        Process: {arch: 'ia32', id: 42, mainModule: {base: ptr(0x400000), name: profile.Sample}},
        retained: [], note: (event, details = {}) => events.push({event, ...details}),
        Memory: {
            alloc, allocUtf8String(text) { return alloc(text.length + 1).writeByteArray(Buffer.from(text + '\0')); },
            patchCode(address, length, action) { patches.push({address: address.value, length}); action(address); }
        },
        NativeFunction: function (address, result, args, options) {
            calls.push({address: address.value, result, args, options});
            return (...values) => natives.get(address.value)?.(...values) ?? ptr(0);
        },
        NativeCallback: function (callback, result, args, abi) { callback.abi = abi; return callback; },
        Interceptor: {
            replace(address, callback) { replacements.set(address.value, callback); },
            replaceFast(address, callback) { replacements.set(address.value, callback); return address; },
            attach(address, callbacks) { attaches.set(address.value, callbacks); return {}; }
        }
    };
    vm.createContext(context);
    for (const name of ['ticket_provider.js', 'connection_handoff.js', 'messenger_adapter.js', 'instance_isolation.js',
                        'warehouse_ui.js', 'shop_features.js', 'configure_runtime.js'])
        vm.runInContext(fs.readFileSync(path.join(directory, name), 'utf8'), context, {filename: name});
    ptr(0x18e7f48).writePointer(alloc(4096));
    const packet = (id, payload, size = payload.length + 8) => {
        const p = alloc(48), begin = alloc(size);
        begin.add(4).writeU16(id); begin.add(8).writeByteArray(payload);
        p.add(24).writePointer(begin); p.add(28).writePointer(begin.add(size)); return p;
    };
    const receive = (id, payload, size, self = ptr(0x123000)) => replacements.get(0x9d2ff0)(self, packet(id, payload, size));
    const options = {Messenger: true, Shop: false, IsolateInstance: false};
    const configure = overrides => context.rpc.exports.configureclient(profile, 'BFS1.' + 'a'.repeat(64), {...options, ...overrides});
    return {ptr, alloc, context, configure, receive, replacements, attaches, calls, natives,
            events, sent, passthrough, patches, packet};
}

const u32 = value => { const b = Buffer.alloc(4); b.writeUInt32LE(value); return b; };
const text = value => Buffer.concat([u32(value.length), Buffer.from(value, 'utf16le')]);
const row = (id, name, kind = 1, online = 1) => Buffer.concat([u32(id), text(name), Buffer.from([kind, online])]);
const roster = (rows, action = 0, status = 0) => Buffer.concat([u32(1), u32(status), Buffer.from([action]), text(''), u32(rows.length), ...rows]);

// A bad late signature must not leave an earlier game hook or shop patch installed.
for (const point of [profile.Ticket, ...profile.Messenger.Points, ...profile.Warehouse, ...profile.Instance]) {
    const f = fixture(); f.ptr(point.Va).writeU8(0);
    throws(() => f.configure({Shop: true, IsolateInstance: true}));
    equal(f.replacements.size, 0); equal(f.patches.length, 0);
}
for (const p of [profile.ShopFeatures.Shop, profile.ShopFeatures.CashPurchase]) {
    const f = fixture(); f.ptr(0x400000 + Number(p.Rva)).writeU8(0);
    throws(() => f.configure({Shop: true})); equal(f.replacements.size, 0);
}
{
    const f = fixture(); f.context.Process.arch = 'x64'; throws(() => f.configure());
    equal(f.replacements.size, 0);
}
{
    const f = fixture(); f.configure(); throws(() => f.configure());
    for (const address of [0xf8f1e0, 0xdace20, 0x9d2ff0]) equal(f.replacements.get(address).abi, 'thiscall');
    for (const address of [0xd72690, 0xdad410, 0xdadbc0, 0xdae670]) {
        const call = f.calls.find(c => c.address === address);
        equal(call.options.abi, 'thiscall'); equal(call.options.traps, 'all');
    }
    const sdk = f.replacements.get(0xf8f1e0), request = f.alloc(10000);
    request.add(12).writeU32(0x110f); request.add(48).writeUtf16String('bad-marker');
    equal(sdk(f.ptr(0), request), 0);
    request.add(48).writeUtf16String('BFS.Ticket'); equal(sdk(f.ptr(0), request), 1);
    equal(request.add(40).readU32(), 1);
    request.add(12).writeU32(0x2105); equal(sdk(f.ptr(0), request), 1);
    equal(request.add(48).readUtf16String(), 'BFS1.' + 'a'.repeat(64));
    request.add(12).writeU32(123); equal(sdk(f.ptr(0), request), 17);
    equal(f.receive(800, Buffer.alloc(0), 1024 * 1024 + 512), 7);
    equal(f.passthrough.length, 1);
    equal(f.receive(170, Buffer.alloc(0)), 7);
    equal(f.receive(170, Buffer.alloc(0)), 7);
    equal(f.sent.length, 1); equal(f.sent[0].id, 65010);
    equal(f.sent[0].payload.toString('hex'), '0100000000000000000000000000000000');
    equal(f.receive(65011, roster([row(55, '好友甲')])), 1);
    equal(f.sent.at(-1).id, 65012);
    equal(f.sent.at(-1).payload, Buffer.concat([u32(1), Buffer.from([0]), u32(0), u32(0), u32(0), text('')]));
    // Native session-member callbacks use the SDK's full 64-bit self id.
    const core = f.alloc(16), model = f.alloc(128), sdkIdentity = f.alloc(256), player = f.alloc(128);
    f.ptr(0x18e57a4).writePointer(core); core.add(4).writePointer(model);
    f.ptr(0x18ebb60).writePointer(sdkIdentity);
    sdkIdentity.add(176).writeU32(0x11223344); sdkIdentity.add(180).writeU32(0x55667788);
    f.ptr(0x18e56e8).writePointer(player); player.add(24).writeUtf16String('本人');
    player.add(40).writeU32(2); player.add(44).writeU32(7);
    let nestedMembers = null;
    f.natives.set(0xdadbc0, () => {
        const nested = f.alloc(160); nested.add(12).writeU32(49155); nested.add(36).writeU32(55);
        equal(sdk(f.ptr(0), nested), 1); nestedMembers = nested.add(120).readPointer(); return 1;
    });
    request.add(12).writeU32(16387); request.add(60).writeUtf16String('好友乙');
    request.add(468).writeUtf16String('请添加'); equal(sdk(f.ptr(0), request), 0);
    equal(f.sent.at(-1).payload, Buffer.concat([u32(1), Buffer.from([2]), u32(0), text('好友乙'), text('请添加')]));
    request.add(12).writeU32(49409); request.add(36).writeU32(55); request.add(1228).writeUtf16String('你好');
    equal(sdk(f.ptr(0), request), 0);
    equal(f.sent.at(-1).payload, Buffer.concat([u32(1), Buffer.from([8]), u32(55), text(''), text('你好')]));
    const before = f.sent.length; request.add(1228).writeUtf16String('x'.repeat(257));
    equal(sdk(f.ptr(0), request), 0); equal(f.sent.length, before);
    request.add(12).writeU32(50000); equal(sdk(f.ptr(0), request), 0);
    equal(f.receive(65015, Buffer.concat([u32(1), u32(77), text('未知'), text('未知'), text('private')])), 0);
    equal(f.receive(65015, Buffer.concat([u32(1), u32(55), text('好友甲'), text('好友甲'), text('你好')])), 1);
    equal(nestedMembers.add(16).readU32(), 0x11223344);
    equal(nestedMembers.add(20).readU32(), 0x55667788);
    equal(nestedMembers.add(24).readUtf16String(), '本人');
    equal(nestedMembers.add(432 + 16).readU32(), 55);
    equal(nestedMembers.add(432 + 20).readU32(), 0);
    // A malformed update cannot erase the previous accepted friend.
    const bad = [Buffer.alloc(0), u32(2), roster([row(55, '甲'), row(55, '乙')]),
                 roster([row(1, '甲', 5)]), roster([row(1, '甲', 1, 2)]), roster([row(0, '甲')]),
                 Buffer.concat([roster([]), Buffer.from([1])]),
                 Buffer.concat([u32(1), u32(0), Buffer.from([0]), text(''), u32(251)])];
    for (const payload of bad) equal(f.receive(65011, payload), 0);
    equal(f.receive(65011, roster([]), 65537), 0);
    equal(f.receive(65015, Buffer.concat([u32(1), u32(55), text('好友甲'), text('好友甲'), text('仍可收到')])), 1);
    check(!JSON.stringify(f.events).includes('BFS1.')); check(!JSON.stringify(f.events).includes('你好'));
    check(!JSON.stringify(f.events).includes('好友甲'));
}
{
    const f = fixture(); f.configure({Shop: true, IsolateInstance: true});
    equal(f.patches.length, 2);
    equal(profile.ShopFeatures.CashPurchase.ReplacementBytes, '90909090909090');
    for (const p of [profile.ShopFeatures.Shop, profile.ShopFeatures.CashPurchase])
        equal(Buffer.from(f.ptr(0x400000 + Number(p.Rva)).readByteArray(p.ReplacementBytes.length / 2)).toString('hex'), p.ReplacementBytes);
    check(f.context.warehouseUiInstalled); check(f.context.instanceIsolationInstalled);
}
{
    const f = fixture();
    let observer, errorCode = 0;
    f.context.Process.attachModuleObserver = value => { observer = value; };
    f.context.Process.setExceptionHandler = () => {};
    f.context.send = value => f.events.push(value);
    vm.runInContext(fs.readFileSync(path.join(directory, 'runtime_guard.js'), 'utf8'), f.context);
    const exports = new Map();
    const addModule = (name, names) => {
        for (const name of names) {
            const address = f.ptr(0x2500000 + exports.size * 16);
            exports.set(name, address); f.natives.set(address.value, () => 23);
        }
        observer.onAdded({name, findExportByName: name => exports.get(name) ?? null,
                          getExportByName: name => exports.get(name)});
    };
    addModule('ws2_32.dll', ['connect', 'WSAConnect', 'sendto', 'WSASendTo', 'getaddrinfo', 'GetAddrInfoW', 'gethostbyname', 'WSASetLastError']);
    f.natives.set(exports.get('WSASetLastError').value, value => { errorCode = value; });
    const endpoint = f.alloc(32); endpoint.writeU16(2); endpoint.add(2).writeByteArray([0x42, 0xd6]);
    const call = name => f.replacements.get(exports.get(name).value);
    for (const [name, index, total] of [['connect', 1, 3], ['WSAConnect', 1, 7], ['sendto', 4, 6], ['WSASendTo', 5, 9]]) {
        const args = Array(total).fill(f.ptr(0)); args[index] = endpoint; args[index + 1] = 16;
        endpoint.add(4).writeU8(127); equal(call(name)(...args), 23);
        endpoint.add(4).writeU8(8); equal(call(name)(...args), -1); equal(errorCode, 10013);
        args[index + 1] = 7; equal(call(name)(...args), -1);
    }
    endpoint.writeByteArray(new Uint8Array(32)); endpoint.writeU16(23); endpoint.add(23).writeU8(1);
    equal(call('connect')(1, endpoint, 28), 23);
    endpoint.add(8).writeU8(0x20); equal(call('connect')(1, endpoint, 28), -1);
    const host = f.alloc(256), result = f.alloc(4);
    host.writeByteArray(Buffer.from('127.0.0.1\0')); equal(call('getaddrinfo')(host, f.ptr(0), f.ptr(0), result), 23);
    host.writeByteArray(Buffer.from('example.com\0')); equal(call('getaddrinfo')(host, f.ptr(0), f.ptr(0), result), 11001);
    check(result.readPointer().isNull()); equal(call('gethostbyname')(host).value, 0);
    host.writeUtf16String('::1'); equal(call('GetAddrInfoW')(host, f.ptr(0), f.ptr(0), result), 23);
    host.writeUtf16String('remote.invalid'); equal(call('GetAddrInfoW')(host, f.ptr(0), f.ptr(0), result), 11001);
    addModule('kernel32.dll', ['CreateProcessW', 'CreateProcessA']);
    equal(call('CreateProcessW')(f.ptr(0)), 0); equal(call('CreateProcessA')(f.ptr(0)), 0);
    addModule('shell32.dll', ['ShellExecuteW', 'ShellExecuteA', 'ShellExecuteExW', 'ShellExecuteExA']);
    equal(call('ShellExecuteW')().value, 5); equal(call('ShellExecuteA')().value, 5);
    equal(call('ShellExecuteExW')(), 0); equal(call('ShellExecuteExA')(), 0);
    addModule('wininet.dll', ['InternetOpenUrlW', 'InternetOpenUrlA']);
    equal(call('InternetOpenUrlW')().value, 0); equal(call('InternetOpenUrlA')().value, 0);
}
// Handoff is opt-in and coexists with either standalone or Messenger dispatch.
for (const messenger of [false, true]) for (const category of [20, 60, 70, 80]) {
    const f = fixture(); f.configure({Handoff: true, Messenger: messenger});
    const source = f.ptr(0x18e7f48).readPointer(), destination = f.alloc(4096);
    equal(f.receive(201, Buffer.alloc(8), undefined, source), 7);
    equal(f.sent.at(-1).id, 65020);
    const announced = f.sent.length;
    equal(f.receive(201, Buffer.alloc(8), undefined, source), 7);
    equal(f.sent.length, announced);
    f.ptr(0x18e7f4c).writePointer(destination);
    const sendHook = f.attaches.get(0x10badc0).onEnter;
    const send = (self, id, payload) => sendHook.call({context: {ecx: self}}, [f.packet(id, payload)]);
    send(source, 53, u32(category));
    equal(f.sent.at(-1).id, 65020); equal(f.sent.at(-1).payload, u32(1));
    const secret = Buffer.from(Array.from({length: 32}, (_, i) => i + 1));
    const grant = Buffer.concat([u32(1), u32(17), u32(category), secret]);
    equal(f.receive(65021, grant, undefined, source), 1);
    const originalSend = f.natives.get(0x961940), connections = [];
    f.natives.set(0x961940, (self, p) => { connections.push(self.value); return originalSend(self, p); });
    send(destination, 54, Buffer.concat([u32(17), u32(1)]));
    equal(f.sent.at(-1).id, 65022);
    equal(f.sent.at(-1).payload, Buffer.concat([u32(1), u32(17), secret]));
    equal(connections.at(-1), destination.value);
    const sent = f.sent.length;
    send(destination, 54, Buffer.concat([u32(17), u32(1)]));
    equal(f.sent.length, sent); // Proof never replays, including duplicate native C54.
    equal(f.receive(9, Buffer.alloc(0), undefined, destination), 7);
    equal(f.receive(11, Buffer.concat([u32(1), u32(category), u32(0)]), undefined, destination), 7);
    check(!JSON.stringify(f.events).includes(secret.toString('hex')));
    // New offer; cancellation restores the source before the native switch.
    equal(f.receive(65021, grant, undefined, source), 1);
    send(destination, 54, Buffer.concat([u32(17), u32(0)]));
    source.add(2504).writeU8(0);
    const closed = [];
    f.natives.set(0x10ba630, p => { closed.push(p.value); });
    equal(f.receive(65023, u32(1), undefined, source), 1);
    equal(source.add(2504).readU8(), 1); equal(closed, [destination.value]);
    const before = f.sent.length;
    send(destination, 54, Buffer.concat([u32(17), u32(0)]));
    equal(f.sent.length, before);
    // Neither a wrong source nor malformed grant reaches native dispatch.
    equal(f.receive(65021, grant, undefined, destination), 0);
    equal(f.receive(65021, grant.subarray(0, 43), undefined, source), 0);
    // Unknown categories have no supported channel destination. Server authority still gates80.
    for (const unsupported of [0, 81, 99, 0xffffffff])
        equal(f.receive(65021, Buffer.concat([u32(1), u32(17), u32(unsupported), secret]), undefined, source), 0);
}
for (const point of profile.Handoff.Points) {
    const f = fixture(); f.ptr(point.Va).writeU8(0);
    throws(() => f.configure({Handoff: true})); equal(f.replacements.size, 0);
}
// Nested group ownership, paired snapshots and native SDK request mapping.
{
    const f=fixture();f.configure();
    const fixed=new Map(fs.readFileSync(path.join(__dirname,'friend_group_packets.txt'),'ascii').trim().split(/\r?\n/).map(line=>{
        const [label,id,body]=line.split(' ');return [label,{id:Number(id),body:Buffer.from(body,'hex')}];
    }));
    const view=(groups,assignments,status=0,action=0)=>Buffer.concat([
        u32(1),u32(status),Buffer.from([action]),u32(groups.length),
        ...groups.map(([id,name])=>Buffer.concat([u32(id),text(name)])),u32(assignments.length),
        ...assignments.map(([friend,group])=>Buffer.concat([u32(friend),u32(group)]))]);
    const core=f.alloc(16),model=f.alloc(128);f.ptr(0x18e57a4).writePointer(core);core.add(4).writePointer(model);
    const destructed=[],freed=[];
    const freeVector=(vector,stride)=>{
        const begin=vector.readPointer(),end=vector.add(4).readPointer(),cap=vector.add(8).readPointer();
        const size=end.sub(begin).toUInt32(),capacity=cap.sub(begin).toUInt32();
        for(let i=0;i<size;i+=stride)destructed.push(begin.add(i).value);
        if(capacity)freed.push((capacity>=4096?begin.sub(4).readPointer():begin).value);
    };
    f.natives.set(0xf96740,(dest,source)=>{dest.writeByteArray(new Uint8Array(source.readByteArray(104)));return dest;});
    f.ptr(0x1631944).writePointer(f.ptr(0xf6dc60));
    f.natives.set(0xf6dc60,(p,flag)=>{equal(flag,0);freeVector(p.add(92),3072);destructed.push(p.value);return p;});
    f.natives.set(0xf6bda0,p=>{p.writePointer(f.ptr(0x1631a84));return p;});
    f.ptr(0x1631a84).writePointer(f.ptr(0x2201000));
    f.natives.set(0x2201000,(p,flag)=>{equal(flag,0);destructed.push(p.value);return p;});
    f.natives.set(0x122ff8b,p=>freed.push(p.value));
    equal(f.receive(170,Buffer.alloc(0)),7);
    equal(f.receive(65011,roster([row(55,'Alpha'),row(56,'Muted',4,0),row(57,'Beta')])),1);
    equal(f.sent.at(-1).payload,fixed.get('list').body);
    equal(f.receive(65013,view([[7,'친구'],[9,'Empty']],[[55,7],[56,7]])),1);
    const rebuild=()=>equal(f.replacements.get(0xdace20)(model),1);
    rebuild();
    let group=model.add(8).readPointer(),friend=group.add(92).readPointer();
    equal(model.add(12).readPointer().sub(group).toUInt32(),208);
    equal(group.readPointer().value,0x1631944);equal(group.add(4).readU16(),25449);
    equal(group.add(6).readU8(),2);equal(group.add(7).readU8(),1);
    equal(group.add(12).readU32(),7);equal(group.add(16).readU32(),74255);
    equal(group.add(20).readU32(),0);equal(group.add(24).readUtf16String(),'친구');
    equal(group.add(96).readPointer().sub(friend).toUInt32(),6144);
    equal(friend.value%32,0);check(!friend.sub(4).readPointer().isNull());
    equal(friend.add(336).readU32(),55);equal(friend.add(360).readUtf16String(),'Alpha');
    equal(friend.add(3072+336).readU32(),56);equal(friend.add(3072+152).readU32(),1);
    equal(group.add(104+12).readU32(),9);check(group.add(104+92).readPointer().isNull());
    equal(model.add(24).readPointer().sub(model.add(20).readPointer()).toUInt32(),3072);
    equal(model.add(20).readPointer().add(336).readU32(),57);
    const old=group.value,oldFriends=friend.value,oldRaw=friend.sub(4).readPointer().value;
    // Roster is staged until its adjacent canonical group snapshot arrives.
    equal(f.receive(65011,roster([row(56,'Muted',4,0),row(57,'Renamed')])),1);
    rebuild();
    equal(model.add(8).readPointer().add(92).readPointer().add(336).readU32(),55);
    check(destructed.includes(old)&&destructed.includes(oldFriends));check(freed.includes(oldRaw)&&freed.includes(old));
    equal(f.receive(65013,view([[7,'친구'],[9,'队友😀']],[[56,7],[57,9]])),1);
    rebuild();group=model.add(8).readPointer();
    check(model.add(20).readPointer().isNull());
    equal(group.add(104+92).readPointer().add(360).readUtf16String(),'Renamed');
    const invalid=[Buffer.alloc(0),u32(2),view([[0,'Zero']],[]),view([[7,'A'],[7,'B']],[]),
        view([[7,'A'],[9,'A']],[]),view([[7,'']],[]),view([[7,'A\0B']],[]),view([[7,'\ud800']],[]),
        view([[7,'G'.repeat(32)]],[]),view([[7,'　']],[]),view([[7,'A']],[[56,9]]),
        view([[7,'A']],[[55,7]]),view([[7,'A']],[[56,7],[56,7]]),view([],[],9),view([],[],0,5),
        Buffer.concat([view([],[]),Buffer.from([0])]),
        Buffer.concat([u32(1),u32(0),Buffer.from([0]),u32(33)]),
        Buffer.concat([u32(1),u32(0),Buffer.from([0]),u32(0),u32(201)])];
    for(const payload of invalid)equal(f.receive(65013,payload),0);
    equal(f.receive(65013,view([],[]),65537),0);
    equal(f.receive(65013,view([],[],8)),1);equal(f.receive(65013,view([],[],7)),1);
    rebuild();equal(model.add(8).readPointer().add(104+24).readUtf16String(),'队友😀');
    // Matching server errors retain the returned authoritative snapshot.
    equal(f.receive(65013,view([[7,'친구'],[9,'队友😀']],[[56,7],[57,9]],6,4)),1);
    const sdk=f.replacements.get(0xf8f1e0),req=f.alloc(256);
    const clear=id=>{req.writeByteArray(new Uint8Array(256));req.add(12).writeU32(id);};
    const invoke=()=>{equal(sdk(f.ptr(0),req),0);equal(req.add(40).readU32(),0);};
    clear(16641);req.add(44).writeU32(74255);req.add(48).writeU32(1);req.add(120).writeU32(1);req.add(52).writeUtf16String('친구');
    invoke();equal(f.sent.at(-1).id,65012);equal(f.sent.at(-1).payload,fixed.get('create').body);
    clear(16643);req.add(44).writeU32(7);req.add(112).writeU32(1);req.add(48).writeUtf16String('队友😀');
    invoke();equal(f.sent.at(-1).payload,fixed.get('rename').body);
    clear(16642);req.add(44).writeU32(7);req.add(48).writeU32(1);
    invoke();equal(f.sent.at(-1).payload,fixed.get('delete_empty').body);
    clear(16391);req.add(44).writeU32(42);req.add(52).writeU32(74255);req.add(60).writeU32(74255);req.add(68).writeU32(7);
    invoke();equal(f.sent.at(-1).payload,fixed.get('assign').body);
    clear(16393);req.add(44).writeU32(42);req.add(52).writeU32(74255);req.add(60).writeU32(74255);req.add(68).writeU32(7);req.add(72).writeU32(9);
    invoke();equal(f.sent.at(-1).payload,fixed.get('move').body);
    const before=f.sent.length;req.add(48).writeU32(1);invoke();equal(f.sent.length,before);
    req.add(48).writeU32(0);req.add(52).writeU32(1);invoke();equal(f.sent.length,before);
    req.add(52).writeU32(74255);req.add(72).writeU32(7);invoke();equal(f.sent.length,before);
    clear(16643);req.add(112).writeU32(1);req.add(48).writeUtf16String('valid');invoke();equal(f.sent.length,before);
    clear(16641);req.add(44).writeU32(74255);req.add(48).writeU32(1);req.add(120).writeU32(1);req.add(52).writeUtf16String('G'.repeat(32));
    invoke();equal(f.sent.length,before);
    clear(16392);req.add(44).writeU32(56);req.add(68).writeU32(7);invoke();
    equal(f.sent.at(-1).id,65010);equal(f.sent.at(-1).payload,Buffer.concat([u32(1),Buffer.from([5]),u32(56),text(''),text('')]));
    // Removing the last friend does not destroy empty named groups.
    equal(f.receive(65011,roster([])),1);equal(f.receive(65013,view([[7,'친구'],[9,'队友😀']],[])),1);
    rebuild();check(model.add(20).readPointer().isNull());check(model.add(8).readPointer().add(92).readPointer().isNull());
    check(!JSON.stringify(f.events).includes('队友😀'));check(!JSON.stringify(f.events).includes('Renamed'));
}
// Durable mailbox adapter: independent wire fixtures, full-width IDs, and fault boundaries.
{
    const fixed=new Map(fs.readFileSync(path.join(__dirname,'offline_message_packets.txt'),'ascii').trim().split(/\r?\n/).map(line=>{
        const [name,id,body]=line.split(' ');return [name,{id:Number(id),body:Buffer.from(body,'hex')}];
    }));
    const f=fixture();f.configure();
    const core=f.alloc(16),model=f.alloc(128);f.ptr(0x18e57a4).writePointer(core);core.add(4).writePointer(model);
    let displayed=0;const seen=[];
    f.natives.set(0xdadbc0,(_,event)=>{displayed++;seen.push([event.add(36).readU32(),event.add(72).readUtf16String(),event.add(1224).readUtf16String()]);return 1;});
    const groupView=Buffer.concat([u32(1),u32(0),Buffer.from([0]),u32(0),u32(0)]);
    equal(f.receive(65017,fixed.get('one').body),0);
    equal(f.receive(65011,roster([row(7,'Alice'),row(8,'친구')])),1);
    equal(f.receive(65013,groupView),1);
    equal(f.sent.at(-1).id,65016);equal(f.sent.at(-1).payload,fixed.get('fetch').body);
    equal(f.receive(65017,fixed.get('one').body),1);equal(displayed,1);
    equal(seen[0],[7,'Alice','Hello']);equal(f.sent.at(-1).payload,fixed.get('ack').body);
    equal(f.receive(65017,fixed.get('one').body),1);equal(displayed,1);
    equal(f.receive(65017,fixed.get('empty').body),1);
    equal(f.receive(65017,fixed.get('one').body),1);equal(displayed,1);
    equal(f.receive(65017,fixed.get('unicode').body),1);equal(displayed,3);
    equal(seen[1],[8,'친구','留言😀']);
    equal(f.sent.at(-1).payload,Buffer.concat([u32(1),Buffer.from([1]),Buffer.from('0100000001000000','hex')]));
    const wide=Buffer.from(fixed.get('one').body);fixed.get('ack_wide').body.copy(wide,12,5,13);
    equal(f.receive(65017,wide),1);equal(displayed,4);equal(f.sent.at(-1).payload,fixed.get('ack_wide').body);
    const before=f.sent.length;
    for(const name of ['unavailable','rate_limited'])equal(f.receive(65017,fixed.get(name).body),1);
    equal(displayed,4);equal(f.sent.length,before);
    const invalid=[];
    for(let i=0;i<fixed.get('one').body.length;i++)invalid.push(fixed.get('one').body.subarray(0,i));
    invalid.push(Buffer.concat([fixed.get('one').body,Buffer.from([0])]));
    const changed=(offset,value)=>{const b=Buffer.from(fixed.get('one').body);b.writeUInt32LE(value,offset);invalid.push(b);};
    changed(0,2);changed(4,3);changed(4,1);changed(8,21);changed(8,0xffffffff);
    changed(12,0);changed(20,99);changed(24,0);changed(24,4102444801);changed(28,1);changed(32,0xffffffff);
    for(const badText of ['\0','\n','\ud800','\udc00','\u202e']) {
        const b=Buffer.from(fixed.get('one').body);b.writeUInt16LE(badText.charCodeAt(0),50);invalid.push(b);
    }
    const repeated=Buffer.concat([u32(1),u32(0),u32(2),fixed.get('one').body.subarray(12),fixed.get('one').body.subarray(12)]);invalid.push(repeated);
    for(const body of invalid)equal(f.receive(65017,body),0);
    equal(displayed,4);equal(f.sent.length,before);
    // A later invalid row cannot cause partial presentation/acks of earlier rows.
    const partial=Buffer.from(fixed.get('unicode').body);partial.writeBigUInt64LE(100n,12);partial.writeBigUInt64LE(101n,52);partial.writeUInt32LE(99,60);
    equal(f.receive(65017,partial),0);equal(displayed,4);equal(f.sent.length,before);
    const fresh=Buffer.from(fixed.get('one').body);fresh.writeBigUInt64LE(9007199254740993n,12);
    core.add(4).writePointer(f.ptr(0));equal(f.receive(65017,fresh),0);equal(f.sent.length,before);
    core.add(4).writePointer(model);
    f.natives.set(0xdadbc0,()=>{throw new Error('Injected native handoff failure');});
    equal(f.receive(65017,fresh),0);equal(f.sent.length,before);
    f.natives.set(0xdadbc0,()=>{displayed++;return 1;});
    const send=f.natives.get(0x961940);
    f.natives.set(0x961940,()=>{throw new Error('Injected ack transport failure');});
    equal(f.receive(65017,fresh),0);equal(displayed,5);equal(f.sent.length,before);
    f.natives.set(0x961940,send);
    equal(f.receive(65017,fresh),1);equal(displayed,5);
    equal(f.sent.at(-1).payload,Buffer.concat([u32(1),Buffer.from([1]),Buffer.from('0100000000002000','hex')]));
    // Paired roster changes must finish before mailbox data can be consumed.
    equal(f.receive(65011,roster([row(7,'Alice'),row(8,'친구')])),1);
    equal(f.receive(65017,fresh),0);equal(f.receive(65013,groupView),1);equal(f.receive(65017,fresh),1);
    const count=f.sent.length;f.attaches.get(0xdb21d0).onLeave();equal(f.sent.length,count+1);
    equal(f.sent.at(-1).payload,fixed.get('fetch').body);
    // The recent handoff cache retains 200 identities, with a documented eviction boundary.
    const record=(id)=>{const body=Buffer.from(fixed.get('one').body.subarray(12));body.writeBigUInt64LE(id,0);return body;};
    const base=0x3000000000000000n;
    for(let offset=0;offset<201;offset+=20) {
        const count=Math.min(20,201-offset);
        const body=Buffer.concat([u32(1),u32(0),u32(count),...Array.from({length:count},(_,i)=>record(base+BigInt(offset+i)))]);
        equal(f.receive(65017,body),1);
    }
    equal(displayed,206);
    equal(f.receive(65017,Buffer.concat([u32(1),u32(0),u32(1),record(base+200n)])),1);equal(displayed,206);
    equal(f.receive(65017,Buffer.concat([u32(1),u32(0),u32(1),record(base)])),1);equal(displayed,207);
    check(!JSON.stringify(f.events).includes('Hello'));check(!JSON.stringify(f.events).includes('留言😀'));
}
console.log(JSON.stringify({checks, passed: true, original_client_started: false, native_execution: false}));
