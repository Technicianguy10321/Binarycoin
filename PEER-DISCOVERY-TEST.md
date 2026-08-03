# BinaryCoin v0.1.5 peer-discovery test

Use three v0.1.5 nodes:

- public seed node at `binarycoin-testnet.ezgateway.net:26001`;
- Windows x64 client;
- Linux ARM64 client.

## Seed node

```bash
./binarycoind -testnet --max-outbound 0 -printtoconsole -debug=net -debug=addrman
```

Keep TCP port 26001 forwarded to the seed. Never forward RPC port 25001.

## Windows and Linux clients

Start both normally, with no private IP and no `-addnode`:

```text
binarycoind -testnet -printtoconsole -debug=net -debug=addrman
```

Each client should first connect to the DNS seed. It then advertises the local
listening endpoint used by that connection. Within roughly 10–20 seconds, the
clients request the refreshed seed address list and one should dial the other.

Check `getpeerinfo`. Each client should retain the public seed peer and at least
one client should show the other device's LAN address as an outbound peer. The
other side shows the matching inbound peer.

Windows Defender Firewall must allow BinaryCoin on Private networks for Windows
to accept inbound LAN sessions. Public-network access is not required.
