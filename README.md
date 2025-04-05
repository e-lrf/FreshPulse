FreshPulse - Aromatizante de Ambiente Automático

O FreshPulse é um projeto desenvolvido para automatizar sprays de aromatizantes de ambiente, permitindo funcionamento autônomo em horários específicos e integração com o Home Assistant (HA). O projeto teve início com o objetivo de reaproveitar um Glade Aparelho Automático que apresentava consumo excessivo de pilhas e seria descartado, transformando-o em uma solução funcional e personalizável.
Sobre o Projeto

Este projeto utiliza hardware acessível e software open-source para criar um aromatizante automático com controle de agendamento e conectividade Wi-Fi. Até o momento, o desenvolvimento e os testes foram concluídos com sucesso, oferecendo as funcionalidades descritas abaixo.
Componentes Utilizados

    1x Wemos D1 Mini
    1x Mini Módulo Regulador de Tensão Step Down Buck DC-DC 3A (com função enable)
    1x Conector Plug Fêmea P4
    1x Cabo USB-A para P4
    1x Fonte de celular USB
    Fios para conexão
    Ferro de solda, solda e fluxo de solda

Ferramentas de Programação

    Visual Studio Code com a extensão PlatformIO
    Linguagens: C++ e HTML

Funcionalidades Atuais

    Realiza sprays automáticos em intervalos configuráveis de 10, 20 ou 30 minutos dentro de um horário definido (ex.: das 8:00 às 22:00).
    Interface web para configuração de Wi-Fi, agendamento e integração com MQTT (opcional).
    Suporte a NTP para sincronização de horário.

Instruções de Instalação
Passo 1: Preparação do Hardware

    Conecte o Wemos D1 Mini ao computador via USB.
    Instale os drivers correspondentes ao Wemos D1 Mini, se necessário.

Passo 2: Configuração do Software

    Clone este repositório do GitHub:
    text

    git clone <URL_DO_REPOSITORIO>
    Abra o projeto no Visual Studio Code com a extensão PlatformIO.
    Carregue o código e o filesystem no Wemos D1 Mini utilizando o PlatformIO.

Passo 3: Configuração Inicial

    Após o upload, o Wemos reiniciará automaticamente e criará uma rede Wi-Fi chamada FreshPulse.
    Conecte-se a essa rede com a senha: freshpulse1234.
    Abra um navegador e acesse: http://192.168.4.1.
    Configure as credenciais da sua rede Wi-Fi e clique em "Salvar". Aguarde cerca de 60 segundos.
    A interface exibirá um endereço mDNS no formato: freshpulse####.local (onde #### são os 4 últimos dígitos do MAC address).

    Atenção: O mDNS funciona apenas em computadores. Em dispositivos Android, será necessário descobrir o IP atribuído ao dispositivo na rede para acessá-lo.

Passo 4: Configuração Final

    Acesse o endereço mDNS (ex.: freshpulse1234.local) ou o IP no navegador.
    Configure o NTP para sincronização de horário.
    Defina o agendamento dos sprays (horário e intervalo).
    (Opcional) Configure o MQTT para integração com o Home Assistant ou outros sistemas.

Configuração de Hardware
Em breve, mais detalhes sobre a montagem física do projeto serão adicionados aqui.

Integração com Home Assistant
Em breve, instruções específicas para integração com o Home Assistant serão incluídas.



Contribuições
Sinta-se à vontade para abrir issues ou enviar pull requests com melhorias, correções ou sugestões! Este é um projeto em desenvolvimento, e toda ajuda é bem-vinda.
