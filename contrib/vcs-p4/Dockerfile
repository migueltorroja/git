FROM ubuntu:16.04

ENV DEBIAN_FRONTEND=noninteractive
RUN echo 'APT::Get::Assume-Yes;' | tee -a /etc/apt/apt.conf.d/00Do-not-ask
RUN apt-get update
RUN apt-get -y upgrade
RUN apt-get -y install apt-utils
RUN apt-get -y install wget
RUN wget -qO - https://package.perforce.com/perforce.pubkey | apt-key add -
RUN echo "deb http://package.perforce.com/apt/ubuntu xenial release" > /etc/apt/sources.list.d/perforce.list
RUN apt-get -y update
RUN apt-get -y install helix-p4d
RUN apt-get -y install gcc make libssl-dev libcurl4-openssl-dev libexpat1-dev gettext python
