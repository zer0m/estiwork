#include "SampleBuilder.h"
#include "..\..\utils\UserSid.h"
#include <sstream>

SampleBuilder::SampleBuilder(Sampler& sampler, MessageSender& sender):
	sampler_(sampler),
	thread_terminated_(false),
	sender_(sender)
{
	fetch_thread = new boost::thread(&SampleBuilder::thread_main, this);
}


SampleBuilder::~SampleBuilder(void)
{
	fetch_thread->detach();
	delete fetch_thread;
}

template <typename T>
void mergequeue(std::queue<T> q1, std::queue<T> & q2)
{
	while(!q1.empty())
	{
		q2.push(q1.front());
		q1.pop();
	}
}

const int NUM_SAMPLES_TO_FETCH = 5;
const int NUM_SAMPLES_TO_GROUP  = 5;
const int NUM_SAMPLES_TO_SEND	= 1;

const int MAX_GROUP_STACK  = 10;

const int THREAD_SLEEP_SECONDS = 0;

// glowna funkcja watku
void SampleBuilder::thread_main()
{
	using namespace boost::posix_time;

	const time_duration wait_seconds = seconds(THREAD_SLEEP_SECONDS);
	time_duration wait_seconds_offset = seconds(0);

	while(!thread_terminated_)
	{
		//std::wcout << "thread wait:" << wait_seconds-wait_seconds_offset << std::endl;
		::boost::this_thread::sleep(std::max<time_duration>(wait_seconds-wait_seconds_offset, seconds(1)));			
		// blokowanie probek samplera
		
		// sampler w osobnym watku zbiera probki co okreslony krotki czas
		// a ten watek pobiera od niego probki w ilosciach NUM_SAMPLES_TO_FETCH
		std::deque<Sample> tmp_stack(sampler_.FetchSamples(NUM_SAMPLES_TO_FETCH));
		int s = tmp_stack.size();
	//	assert(s);
	//	assert(tmp_stack[0].probe_time_.is_initialized());
		sample_stack.insert(sample_stack.end(), tmp_stack.begin(), tmp_stack.end());				

		ptime start_time, end_time;
		start_time = second_clock::universal_time();

		// ANALIZA, GRUPOWANIE, SCALANIE
		// NUM_SAMPLES_TO_GROUP okresla ile probek zostanie scalonych i wyslanych.
		// 
		if(sample_stack.size() >= NUM_SAMPLES_TO_GROUP)
		{
			assert(sample_stack.size()>0);
			assert(sample_stack[0].probe_time_.is_initialized());

			std::deque<Sample> s = GroupStack(sample_stack); // groupuje wedlug programow


			// test czy wszystkie Sample zgrupowane sa tego samego handlera czyli tej samej aplikacji
			assert(std::adjacent_find(s.begin(), s.end(), [](Sample& a, Sample &b){ return b.window_handle_ != a.window_handle_; }) == s.end());



			SampleMessage new_merged = GroupMerge(s);

			std::stringstream ss;
			ss << "Sample:"
				<< boost::posix_time::to_simple_string(new_merged.sample.probe_time_.get())
				<< "    " << new_merged.duration.seconds();
	
			pantheios::log_INFORMATIONAL(ss.str());


			sample_stack_for_send.push_back(new_merged);
		}

		// WYSYLANIE
		// wysylanie zgrupowanych sampli
		// nie jest konieczne wrzucanie zgrupowanych sampli od razu do bazy wiec mozna je buforowac
		if(sample_stack_for_send.size() >= NUM_SAMPLES_TO_SEND)
		{

			/*assert(std::adjacent_find(sample_stack_for_send.begin(), sample_stack_for_send.end(), [](SampleMessage& a, SampleMessage& b){
				return a.sample.probe_time_.get() + a.duration == a.sample.probe_time_.get();
			}) != sample_stack_for_send.end());*/
			
			assert(std::adjacent_find(sample_stack_for_send.begin(), sample_stack_for_send.end(), [](SampleMessage& a, SampleMessage& b) -> bool {								
				std::stringstream ss;
				ss << b.sample.probe_time_.get() << " == " << (a.sample.probe_time_.get() + a.duration) << " (" << a.sample.probe_time_.get() << "+" << a.duration;
				pantheios::log_INFORMATIONAL(ss.str());

				return a.sample.probe_time_.get() + a.duration != b.sample.probe_time_.get();		
			}) == sample_stack_for_send.end());

			


			sender_.SendSamples(sample_stack_for_send);
		}

		end_time = second_clock::universal_time();

		wait_seconds_offset = (end_time - start_time);
		wait_seconds_offset = std::max<time_duration>(wait_seconds_offset, seconds(0));
		
		//std::wcout << "thread_main" << std::endl;
	}
}

/*
Z zebranych danych przygotowuje dane do zapisania do bazy
- grupuje programy wedlug uchwytu; 
	zakladamy ze jesli uruchomi sie program ponownie i zmieni naturalnie uchwyt to bedzie to po prostu nowy wpis
- i wedlug aktywnosci tj. ciaglosc aktywnosci jest wrzucana osobno niz ciaglosc idlowania

	np. lista<Sample>
	- prog1  -
	- prog2  - zapisuje prog 1
	- prog3  - zapisuje prog 2
	- prog3  - dalej
	- prog3  - dalej
	- prog2  - zapisuje prog 3
	
	z tego tworzy paczki do wyslania
*/
std::deque<Sample> SampleBuilder::GroupStack(std::deque<Sample>& stack)
{
	std::deque<Sample> for_return;

	const int analyze_num = MAX_GROUP_STACK;
	int analyse_count = 0;
	boost::optional<Sample> sample_last;
	while(!stack.empty() && analyse_count++<analyze_num) 
	{

	//	Sample for_merge;
		Sample sample = stack.front();
		
		/*if(!sample_last.is_initialized())
			stack.pop_front();*/
		assert(sample.probe_time_.is_initialized());
		if(!sample_last.is_initialized() || IsSameSampleTest(sample_last.get(), sample))
			 // PRAWDA=ten sam program
		{
		//	std::wcout << "s:" << sample.sample_id_ << std::endl;
			assert(sample.probe_time_.is_initialized());
			for_return.push_back(sample);
			stack.pop_front();
			assert(sample.probe_time_.is_initialized());

		}
		else // inny program
		{
		//	std::wcout << "inny" << std::endl;
			break;
		}

		sample_last = sample;
	}

	return for_return;
}

// metoda zaklada ze Sample w stack sa tego samego typu(tej samej aplikacji)
// TODO: w wielu miejscach programu(w testach tez) brakuje parametryzowania okresu czasu probkowania
// tu np. jest + seconds(1); a powinno byc +T; gdzie T to okres czasu probkowania

// teraz jest zapis w postaci   STARSZA DATA + DURATION
// jesli bdzie potrzebny zapis najnowszej daty + duration to trzeba usunac dodanie sekundy
SampleMessage SampleBuilder::GroupMerge(std::deque<Sample>& stack)
{

	assert(stack.front().probe_time_.get() <= stack.back().probe_time_.get());
	assert(stack.empty() == false);
	assert(stack[0].probe_time_.is_initialized());

	using namespace boost::posix_time;

	time_duration duration;
	std::cout << " ------ Group merge --------" << std::endl;
	std::for_each(stack.begin(), stack.end(), [](Sample& s){
		std::cout << s.probe_time_.get() << std::endl;
	});
	


	if (&stack.back() == &stack.front())
	{
		duration = seconds(1); // bo probkowanie trwa jedna sekunde
		/*
			jesli probkowanie trwa 1 sekunde to kolejne probkowanie bedzie za jedna sekunde wiec punktem w czasie jest 1 sek
			nawet jesli mamy jednen sample tylko
		*/
	}
	else
	if(stack.back().probe_time_.is_initialized() &&
		stack.front().probe_time_.is_initialized())
	{
		duration = stack.back().probe_time_.get() - stack.front().probe_time_.get() + seconds(1);
	}
	/*std::wcout << L"GroupMerge:" << 
		stack.back().probe_time_.get() << "-" << stack.front().probe_time_.get() << std::endl;*/

	SampleMessage merged;
	merged.duration = duration;
	merged.sample =  stack.front(); // front to starsza data czyli    STARSZA DATA + DURATION == NAJNOWSZA DATA
	merged.usersid = UserSid::GetUserSID();
	//GetLogonSID()
	//merged.usersid = GetLogonStringSID()

	std::wstringstream logStr;
	logStr << "GroupMerge:"
		<< merged.sample.probe_time_.get() << ", " << merged.duration;
	
	pantheios::log_DEBUG("GroupMerge:", pantheios::w2m(logStr.str()));
	
	//std::cout << " ------ end Group merge --------" << duration << std::endl;

	return merged;
}

bool SampleBuilder::IsSameSampleTest(Sample& a, Sample& b)
{
	bool handles = a.window_handle_ == b.window_handle_;
	bool idle = a.idle == b.idle;
	bool image = a.image_full_path_ == b.image_full_path_;

	if(handles && idle)
		return true;
	else
		return false;
}