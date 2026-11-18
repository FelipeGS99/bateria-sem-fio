# Bateria sem fio

Mostra na bandeja do Windows a porcentagem de bateria de cada dispositivo compatível. A leitura acontece ao abrir e se repete uma vez por minuto.

## Como usar

Baixe o arquivo `BateriaSemFio.zip` na página da versão mais recente. [Releases](https://github.com/FelipeGS99/bateria-sem-fio/releases/latest)

1. Extraia o ZIP. Abra `BateriaSemFio.exe` no Windows 10 ou 11. Em um computador com Windows em ARM, use `BateriaSemFio-arm64.exe`.
2. Conecte os receptores USB ou pareie seus dispositivos Bluetooth. O número no ícone é a porcentagem. Passe o mouse para ver o nome ou clique em qualquer ícone para ver a lista e usar **Atualizar agora** ou **Sair**.
3. Se os ícones aparecerem sob a seta **^**, arraste-os para perto do relógio ou habilite-os nas configurações da barra de tarefas.

O aplicativo é portátil e roda localmente, sem instalação, acesso à internet ou permissões de administrador.

## Compatibilidade

O app consulta a bateria destes modelos pelo receptor USB de 2,4 GHz.

- **HyperX Cloud III Wireless**. [Protocolo do receptor](https://github.com/LennardKittner/HyperHeadset/blob/main/src/devices/cloud_iii_wireless.rs)
- **Logitech G PRO X SUPERLIGHT**. [Identificação do receptor LIGHTSPEED](https://github.com/pwr-Solaar/Solaar/issues/3036)
- **Logitech G PRO X TKL LIGHTSPEED**. [Recurso de bateria HID++ do teclado](https://github.com/pwr-Solaar/Solaar/issues/2958)

O app usa consultas HID de bateria nos receptores USB compatíveis. Se o periférico estiver desligado, adormecido ou sem uma porcentagem disponível, seu ícone pode sumir até a próxima leitura.

### Bluetooth

O aplicativo consulta as porcentagens que os drivers Bluetooth informam ao Windows. A indicação **Windows** na dica do ícone identifica esse valor, que pode corresponder à última leitura registrada pelo sistema. Para dispositivos Bluetooth LE sem esse valor, o aplicativo tenta ler a característica **Battery Level** do serviço padrão **Battery Service**. Se a leitura não estiver disponível, o nível não aparece. Se várias leituras da mesma fonte forem agrupadas para um único dispositivo, o ícone exibe a menor.

## Código-fonte

Na pasta `codigo-fonte`, `app.cpp` implementa a bandeja, `battery.cpp` consulta o Bluetooth e `usb_battery.cpp` lê os níveis via USB HID.

Para recompilar, abra a pasta `codigo-fonte` e use Zig 0.15.2. No Linux com terminal POSIX, execute `./build.sh` para x64 ou `./build.sh aarch64-windows-gnu BateriaSemFio-arm64.exe` para ARM64. No Windows, rode os dois comandos abaixo no PowerShell, na pasta do código.

```powershell
zig rc /fo app.res app.rc
zig c++ -target x86_64-windows-gnu -std=c++20 -Os -DNDEBUG -DUNICODE -D_UNICODE -static -s -municode '-Wl,--subsystem,windows' app.cpp battery.cpp usb_battery.cpp app.res -o BateriaSemFio.exe -lsetupapi -lcfgmgr32 -lbluetoothapis -lole32 -lshell32 -luser32 -lgdi32 -lhid
```
