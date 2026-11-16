# Multistream OBS

Um plugin para multistream no OBS, sem frufru. Transmita para a Twitch e a Kick enquanto a transmissão principal do OBS continua configurada para o YouTube. Cada destino do plugin tem controles e configurações próprios.

> **Situação do projeto:** a transmissão para um servidor RTMP local foi testada. A conexão com contas reais da Twitch e da Kick e a estabilidade em transmissões longas ainda precisam ser verificadas.

## Recursos

- Saídas independentes para Twitch e Kick, cada uma com botão de iniciar e parar.
- Configuração separada de codificador, resolução, taxa de bits, FPS, cena e faixa de áudio para cada destino.
- Opções independentes para iniciar e parar junto com a transmissão principal do OBS.
- Verificação automática de novas versões. Quando uma atualização é instalada, ela passa a funcionar ao reiniciar o OBS, sem interromper a transmissão atual.
- Atalhos configuráveis no OBS e botões para iniciar ou parar todos os destinos.

O plugin é destinado à transmissão. Ele não inclui chat nem login por conta.

## Requisitos

- Windows 10 ou 11 de 64 bits.
- OBS Studio **33.0.1**. Outras versões não foram testadas.
- Conexão de upload suficiente para a soma das taxas de todas as transmissões, com margem para variações.

Codificadores independentes exigem mais recursos do processador ou da GPU. O limite de sessões do codificador também depende do hardware e dos drivers.

## Instalação

1. Baixe o arquivo `MultistreamOBS-*-Windows-x64.zip` na página de [versões](https://github.com/GuiCraftBR-eu/MultistreamOBS/releases).
2. Feche o OBS.
3. Extraia a pasta `multistream-obs` para `C:\ProgramData\obs-studio\plugins\`. Ao final, o arquivo deve estar em `C:\ProgramData\obs-studio\plugins\multistream-obs\multistream-obs.dll`.
4. Abra o OBS. Se o painel não aparecer, procure **Multistream** no menu **Docks**.

Para o OBS portátil, extraia a mesma pasta `multistream-obs` para a pasta `plugins` que fica na raiz da instalação portátil. Não misture a estrutura nova com pastas antigas `bin/64bit`.

## Configuração

1. Configure o YouTube normalmente em **Configurações → Transmissão** do OBS.
2. No painel Multistream, clique em **Adicionar novo destino → Twitch**. Cole a chave de transmissão no campo correspondente. O servidor RTMPS da Twitch já vem preenchido, mas pode ser alterado.
3. Adicione um destino **Kick** e copie a URL do servidor e a chave da página de configurações de transmissão do painel da Kick. Use a URL mostrada na sua conta.
4. Ajuste vídeo e áudio para cada destino. Por padrão, um novo destino usa x264 separado a 6.000 Kbps, CBR e quadros-chave a cada dois segundos, além de AAC separado a 160 Kbps. Escolha valores e codificadores adequados para seu computador e para cada plataforma.
5. Use **Iniciar** e **Parar** em cada destino. Para acompanhar a transmissão principal, marque **Sincronizar início com o OBS** e/ou **Sincronizar parada com o OBS** nas configurações do destino. O início sincronizado ocorre após a transmissão principal iniciar com sucesso.

A opção de reutilizar o codificador do OBS compartilha as configurações e pode depender de a transmissão principal estar ativa. Use codificadores separados para controlar os destinos independentemente.

**Proteja suas chaves:** elas aparecem mascaradas na interface, mas são gravadas em texto simples na configuração do perfil do OBS. Arquivos exportados pelo plugin também contêm essas chaves; não os compartilhe.

## Atualizações automáticas

No Windows de 64 bits, o plugin verifica a última versão estável deste repositório ao iniciar e, enquanto o OBS permanecer aberto, a cada 12 horas. Uma versão mais recente é baixada do GitHub e verificada pelo SHA-256 informado na API da versão antes da instalação. A atualização só é aplicada para OBS 33 e entra em uso na próxima inicialização do OBS; o plugin não reinicia nem encerra uma transmissão automaticamente.

O plugin precisa de permissão para gravar na própria pasta. Se a atualização falhar, o painel mostra uma mensagem e o usuário pode instalar o ZIP manualmente após fechar o OBS. A verificação usa HTTPS e confia nas versões publicadas neste repositório; o SHA-256 protege a integridade do download, mas não substitui uma assinatura digital independente.

## Compilação no Windows

São necessários CMake 3.28 ou superior, Visual Studio 2022 Build Tools com C++, Windows SDK e Python 3. O projeto baixa as dependências do OBS e do Qt informadas em `buildspec.json` e verifica seus hashes.

```powershell
cmake --preset windows-x64 -DMULTISTREAM_BUILD_TESTS=ON
cmake --build --preset windows-x64 --parallel 4
$env:PATH = "$PWD\.deps\obs-deps-qt6-2026-10-23-x64\bin;$env:PATH"
ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure
cmake --install build_x64 --config RelWithDebInfo --prefix "$PWD\dist\stage"
```

O fluxo de trabalho do GitHub compila, executa os testes e publica uma versão quando `buildspec.json` contém um número de versão estável que ainda não existe. O arquivo `RELEASE_NOTES.md` fornece as notas da versão; revise-o antes de alterar a versão.

## Créditos e licença

Este projeto deriva de [obs-multi-rtmp, de SoraYuki e colaboradores](https://github.com/sorayuki/obs-multi-rtmp), e mantém sua licença [GNU GPL v2](LICENSE). As alterações incluem identidade e configuração separadas, atalhos para Twitch e Kick, sincronização após o início da transmissão principal, estrutura de plugin do OBS 33 e atualizações automáticas. O código original e os avisos de licença de terceiros são preservados.

O pacote do Windows também inclui o backend Schannel do Qt 6.11.1, distribuído com sua licença em `licenses/qt6/LICENSE.GPL2`. O código-fonte correspondente está disponível no [repositório oficial do Qt](https://code.qt.io/cgit/qt/qtbase.git/tag/?h=v6.11.1) e na [distribuição de fontes do Qt 6.11.1](https://download.qt.io/official_releases/qt/6.11/6.11.1/submodules/).
